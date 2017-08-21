#pragma once

#include <stdio.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <iostream>
#include <string>
#include <was/storage_account.h>
#include <was/table.h>

const utility::string_t AzureConnectionString(U("DefaultEndpointsProtocol=https;AccountName=andiry;AccountKey=GEdd85kJS4sWnzfp+t8ikhsgbr1QPqZy0ZbocPI2fKGdiBhB8Agom9UwOW8OUvWpFq7bcOdnT34O/woiSY6W2w==;EndpointSuffix=core.windows.net"));

const utility::string_t OCSSDResourceTableName(U("OCSSDResource2"));

std::string get_ip();
int insert_entity(const std::string &str);
int retrieve_entity();

