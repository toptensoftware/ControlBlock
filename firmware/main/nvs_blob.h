#pragma once

// BlobStorage over NVS, device layer (hardware only). One blob per
// namespace + key; nvs_flash_init() must have run.

#include "blob_storage.h"

// Where a blob lives. Must outlive the storage (keep it static). `what`
// names the blob in error logs ("the credentials").
struct NvsBlob
{
    const char *ns;
    const char *key;
    const char *what;
};

BlobStorage nvs_blob_storage(const NvsBlob *blob);
