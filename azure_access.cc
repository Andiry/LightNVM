#include "azure_config.h"

std::string get_ip()
{
	struct ifaddrs *ifAddrStruct = NULL;
	struct ifaddrs *ifa = NULL;
	void * tmpAddrPtr = NULL;
	std::string prefix("eno1");
	std::string ret;

	getifaddrs(&ifAddrStruct);

	for (ifa = ifAddrStruct; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr)
			continue;

		/* Only get IPv4 addr */
		if (ifa->ifa_addr->sa_family == AF_INET) {
			char addressBuffer[INET_ADDRSTRLEN];
			tmpAddrPtr = &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
			inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);
			if (!prefix.compare(0, prefix.size(), ifa->ifa_name)) {
				ret = addressBuffer;
				break;
			}
		}
	}

	if (ifAddrStruct)
		freeifaddrs(ifAddrStruct);

	return ret;
}

int insert_entity(const std::string &device, size_t numShared, size_t numExclusive, size_t freeBlocks)
{
	azure::storage::table_entity entity(U("OCSSD"), U(device));

	azure::storage::table_entity::properties_type &properties = entity.properties();
	properties.reserve(3);

	properties[U("NumSharedChannels")] = azure::storage::entity_property(U(std::to_string(numShared)));
	properties[U("NumExclusiveChannels")] = azure::storage::entity_property(U(std::to_string(numExclusive)));
	properties[U("FreeBlocks")] = azure::storage::entity_property(U(std::to_string(freeBlocks)));
	azure::storage::table_operation insert_operation =
		azure::storage::table_operation::insert_or_replace_entity(entity);

	auto StorageAccount = azure::storage::cloud_storage_account::parse(AzureConnectionString);
	auto TableClient = StorageAccount.create_cloud_table_client();
	auto Table = TableClient.get_table_reference(OCSSDResourceTableName);
	Table.create_if_not_exists();
	azure::storage::table_result insert_result = Table.execute(insert_operation);

	return 0;
}

int retrieve_entity()
{
	azure::storage::table_query query;

	query.set_filter_string(azure::storage::table_query::generate_filter_condition(U("PartitionKey"),
				azure::storage::query_comparison_operator::equal, U("OCSSD")));

	auto StorageAccount = azure::storage::cloud_storage_account::parse(AzureConnectionString);
	auto TableClient = StorageAccount.create_cloud_table_client();
	auto Table = TableClient.get_table_reference(OCSSDResourceTableName);
	Table.create_if_not_exists();
	azure::storage::table_query_iterator it = Table.execute_query(query);

	azure::storage::table_query_iterator end_of_results;
	for (; it != end_of_results; ++it) {
		const azure::storage::table_entity::properties_type &properties = it->properties();
		std::cout << U("PartitionKey: ") << it->partition_key().c_str()
			<< U(", Device: ") << it->row_key().c_str()
			<< U(", NumSharedChannels: ") << properties.at(U("NumSharedChannels")).string_value().c_str()
			<< U(", NumExclusiveChannels: ") << properties.at(U("NumExclusiveChannels")).string_value().c_str()
			<< U(", FreeBlocks: ") << properties.at(U("FreeBlocks")).string_value().c_str() << std::endl;
	}

	return 0;
}

#if 0
int main()
{
	std::cout << get_ip() << std::endl;
	insert_entity("Test");
	insert_entity("Test1");
	retrieve_entity();
}
#endif
