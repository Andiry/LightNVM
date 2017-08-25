#pragma once

#include <stdio.h>
#include <sys/types.h>
#include <iostream>
#include <string>
#include <was/storage_account.h>
#include <was/table.h>

const utility::string_t AzureConnectionString(U("DefaultEndpointsProtocol=https;AccountName=andiry;AccountKey=4BSC+b2+8OJJQJ4ehoa2deoCaUbxbVVaCPQOuWF3qFKcfA6dV099FgmsJb9sBwG/saXV7XDPQrxj+NiAZ7YDzg==;EndpointSuffix=core.windows.net"));

const utility::string_t OCSSDResourceTableName(U("OCSSDResource"));

int azure_insert_entity(const std::string &device, size_t numShared, size_t numExclusive, size_t freeBlocks);
int azure_retrieve_entity();
int azure_delete_entities();

