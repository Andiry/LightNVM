#include <stdio.h>
#include <iostream>
#include <string>
#include <was/storage_account.h>
#include <was/table.h>

const utility::string_t AzureConnectionString(U("DefaultEndpointsProtocol=https;AccountName=andiry;AccountKey=GEdd85kJS4sWnzfp+t8ikhsgbr1QPqZy0ZbocPI2fKGdiBhB8Agom9UwOW8OUvWpFq7bcOdnT34O/woiSY6W2w==;EndpointSuffix=core.windows.net"));

const utility::string_t OCSSDResourceTableName(U("OCSSDResource2"));

static int insert_entity(const char * str)
{
	azure::storage::table_entity entity(U("OCSSD"), U(str));

	azure::storage::table_entity::properties_type &properties = entity.properties();
	properties.reserve(2);

	properties[U("Email")] = azure::storage::entity_property(U("t-jianxu@microsoft.com"));
	properties[U("Phone")] = azure::storage::entity_property(U("8589006842"));
	azure::storage::table_operation insert_operation =
		azure::storage::table_operation::insert_or_replace_entity(entity);

	auto StorageAccount = azure::storage::cloud_storage_account::parse(AzureConnectionString);
	auto TableClient = StorageAccount.create_cloud_table_client();
	auto Table = TableClient.get_table_reference(OCSSDResourceTableName);
	Table.create_if_not_exists();
	azure::storage::table_result insert_result = Table.execute(insert_operation);

	return 0;
}

static int retrieve_entity()
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
			<< U(", RowKey: ") << it->row_key().c_str()
			<< U(", Property1: ") << properties.at(U("Email")).string_value().c_str()
			<< U(", Property2: ") << properties.at(U("Phone")).string_value().c_str() << std::endl;
	}

	return 0;
}

int main()
{
	insert_entity("Test");
	insert_entity("Test1");
	retrieve_entity();
}
