#pragma once

#include <stdio.h>
#include <sys/types.h>
#include <iostream>
#include <string>
#include <was/storage_account.h>
#include <was/table.h>

const utility::string_t AzureConnectionString(U("DefaultEndpointsProtocol=https;AccountName=andiry;AccountKey=GEdd85kJS4sWnzfp+t8ikhsgbr1QPqZy0ZbocPI2fKGdiBhB8Agom9UwOW8OUvWpFq7bcOdnT34O/woiSY6W2w==;EndpointSuffix=core.windows.net"));

const utility::string_t OCSSDResourceTableName(U("OCSSDResource"));

int azure_insert_entity(const std::string &device, size_t numShared, size_t numExclusive, size_t freeBlocks);
int azure_retrieve_entity();
int azure_delete_entities();

