import os
import numpy as np
from tqdm import tqdm
from datasets import load_dataset
from tokenizers import Tokenizer, models, pre_tokenizers, decoders, trainers
from transformers import PreTrainedTokenizerFast

# ================= KONFIGURASI =================
DATASET_NAME   = "HuggingFaceFW/fineweb-edu"
DATASET_CONFIG = "sample-10BT"
TEXT_COLUMN    = "text"

# Tokenizer
VOCAB_SIZE = 16_384            # 128 * 128 -> kelipatan 128
TRAIN_DOCS = 150_000           # jumlah dokumen untuk melatih BPE
SAVE_DIR   = "./custom_tokenizer"

# Dataset
TARGET_TRAIN_TOKENS = 2_500_000_000   # 25x parameter (model 100M)
TARGET_VAL_TOKENS   = 10_000_000      # ukuran val maksimum
VAL_EVERY           = 200             # tiap dokumen ke-200 masuk val (~0.5%)
TRAIN_FILE = "train.bin"
VAL_FILE   = "val.bin"

BATCH_SIZE     = 1000
VAL_BATCH_SIZE = 200

os.environ.setdefault("TOKENIZERS_PARALLELISM", "true")


def load_stream():
    return load_dataset(
        DATASET_NAME, name=DATASET_CONFIG, split="train", streaming=True
    )


def is_val(doc_idx):
    """Split per DOKUMEN (bukan per token) supaya tidak ada kebocoran train -> val."""
    return doc_idx % VAL_EVERY == 0


def doc_stream():
    for i, ex in enumerate(load_stream()):
        text = ex.get(TEXT_COLUMN, "")
        if text:
            yield i, text


# ---------------------------------------------------------------
# STEP 1: Latih tokenizer (hanya dari dokumen non-val)
# ---------------------------------------------------------------
def tokenizer_batches(max_docs, batch_size=1000):
    batch, n = [], 0
    for i, text in doc_stream():
        if is_val(i):
            continue  # dokumen val tidak boleh ikut melatih tokenizer
        batch.append(text)
        n += 1
        if len(batch) >= batch_size:
            yield batch
            batch = []
        if n >= max_docs:
            break
    if batch:
        yield batch


def train_tokenizer():
    print(f"[step 1] Melatih tokenizer BPE (vocab={VOCAB_SIZE}) dari {TRAIN_DOCS:,} dokumen...")

    tokenizer = Tokenizer(models.BPE())
    tokenizer.pre_tokenizer = pre_tokenizers.ByteLevel(add_prefix_space=False)
    tokenizer.decoder = decoders.ByteLevel()

    trainer = trainers.BpeTrainer(
        vocab_size=VOCAB_SIZE,
        special_tokens=["<|endoftext|>", "<|pad|>"],
        initial_alphabet=pre_tokenizers.ByteLevel.alphabet(),
        show_progress=True,
    )

    tokenizer.train_from_iterator(tokenizer_batches(TRAIN_DOCS), trainer=trainer)

    assert tokenizer.get_vocab_size() == VOCAB_SIZE, (
        f"Vocab size {tokenizer.get_vocab_size()} != {VOCAB_SIZE}"
    )

    hf_tokenizer = PreTrainedTokenizerFast(
        tokenizer_object=tokenizer,
        eos_token="<|endoftext|>",
        pad_token="<|pad|>",
    )
    hf_tokenizer.save_pretrained(SAVE_DIR)
    print(f"[step 1 selesai] Tokenizer tersimpan di '{SAVE_DIR}'")


# ---------------------------------------------------------------
# STEP 2: Tokenisasi dataset -> train.bin + val.bin
# ---------------------------------------------------------------
def encode_and_write(tok, batch, fh, eot_id, dtype, remaining):
    """Encode batch, tambah EOS tiap dokumen, tulis max `remaining` token."""
    if remaining <= 0 or not batch:
        return 0
    encoded = tok.encode_batch(batch, add_special_tokens=False)
    flat = []
    for enc in encoded:
        flat.extend(enc.ids)
        flat.append(eot_id)
    arr = np.array(flat, dtype=dtype)[:remaining]
    fh.write(arr.tobytes())
    return len(arr)


def tokenize_dataset():
    tok = Tokenizer.from_file(os.path.join(SAVE_DIR, "tokenizer.json"))
    vocab_size = tok.get_vocab_size()
    eot_id = tok.token_to_id("<|endoftext|>")
    assert eot_id is not None, "EOS token tidak ditemukan di tokenizer"

    dtype = np.uint16 if vocab_size <= 65535 else np.uint32
    print(f"[step 2] Vocab: {vocab_size} | Dtype: {dtype.__name__} | "
          f"Train: {TARGET_TRAIN_TOKENS:,} | Val maks: {TARGET_VAL_TOKENS:,}")

    train_written = 0
    val_written = 0
    train_batch, val_batch = [], []
    pbar = tqdm(total=TARGET_TRAIN_TOKENS, unit="tok", unit_scale=True, desc="Tokenizing train")

    with open(TRAIN_FILE, "wb") as f_train, open(VAL_FILE, "wb") as f_val:

        def flush_val():
            nonlocal val_written, val_batch
            val_written += encode_and_write(
                tok, val_batch, f_val, eot_id, dtype, TARGET_VAL_TOKENS - val_written
            )
            val_batch = []

        def flush_train():
            nonlocal train_written, train_batch
            n = encode_and_write(
                tok, train_batch, f_train, eot_id, dtype, TARGET_TRAIN_TOKENS - train_written
            )
            train_written += n
            pbar.update(n)
            train_batch = []

        for i, text in doc_stream():
            if is_val(i):
                if val_written < TARGET_VAL_TOKENS:
                    val_batch.append(text)
                    if len(val_batch) >= VAL_BATCH_SIZE:
                        flush_val()
            else:
                train_batch.append(text)
                if len(train_batch) >= BATCH_SIZE:
                    flush_train()
                    if train_written >= TARGET_TRAIN_TOKENS:
                        break

        # sisa batch terakhir
        flush_val()
        if train_written < TARGET_TRAIN_TOKENS:
            flush_train()

    pbar.close()

    gb = lambda p: os.path.getsize(p) / (1024 ** 3)
    print(f"[step 2 selesai]")
    print(f"  train: {train_written:,} token -> '{TRAIN_FILE}' ({gb(TRAIN_FILE):.2f} GB)")
    print(f"  val  : {val_written:,} token -> '{VAL_FILE}' ({gb(VAL_FILE):.3f} GB)")

    if train_written < TARGET_TRAIN_TOKENS:
        print(f"[warning] Dataset habis sebelum target train tercapai "
              f"({train_written:,} < {TARGET_TRAIN_TOKENS:,})")
    if val_written < TARGET_VAL_TOKENS:
        print(f"[info] Val {val_written:,} < target {TARGET_VAL_TOKENS:,}. "
              f"Kecilkan VAL_EVERY kalau mau val lebih besar.")


def main():
    if os.path.exists(os.path.join(SAVE_DIR, "tokenizer.json")):
        print(f"[skip] Tokenizer sudah ada di '{SAVE_DIR}', lewati training.")
    else:
        train_tokenizer()
    tokenize_dataset()


if __name__ == "__main__":
    main()
