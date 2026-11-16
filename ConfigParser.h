#ifndef _CONFIGPARSER_H_
#define _CONFIGPARSER_H_
#include "htypes.h"
#include <string>
#include <vector>

/**
 * Class performing config file processing
 */

class ConfigParser
{
	public:
		/**
		 * Object init
		 */
		ConfigParser();

		/**
		 * Reads a config file into the object structures
		 * @param filename Name of config file. Will be opened for reading only.
		 * @return true if config file was successfully read
		 */
		bool parseConfig(const char *filename);

		/**
		 * Reads an integer parameter from the config file
		 * If the parameter was not specified, the default is used
		 * @param key Parameter name
		 * @param defaultval Default value if parameter was not found
		 * @return Parameter value
		 */
		uint64_t getInt(std::string key, uint64_t defaultval=0) const;

		/**
		 * Reads a parameter containing a comma separated list of non-negative integers,
		 * e.g. "QuantileIndices=0,4,8". An empty list is returned if the parameter was not found.
		 * @param key Parameter name
		 * @return List of integer values
		 */
		const std::vector<uint64_t> &getIntList(std::string key) const;

		/**
		 * Reads a parameter containing sorting network pairs from the config file
		 * An empty network is returned if the parameter was not found
		 * @param key Parameter name
		 * @return Network composed of CEs
		 */
		const Network_t &getNetwork(std::string key) const;

		/**
		 * Clean up
		 */
		~ConfigParser();
	private:
		class Data;
		Data *data;
};

#endif // _CONFIGPARSER_H_
