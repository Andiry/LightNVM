#include "azure_config.h"

int azure_insert_entity(const std::string &device, size_t numShared, size_t numExclusive, size_t freeBlocks)
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

int azure_retrieve_entity()
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
