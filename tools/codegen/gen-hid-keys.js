#!/usr/bin/env node
// node tools/codegen/gen-hid-keys.js
// Writes firmware/main/hid_keys.gen.h from the bake's key table (host/src/bake/hid.js).
// The text comes from host/src/bake/headers.js; host's test/bake/hid-keys-header.test.js fails if
// the committed file is out of date.

import { writeHeader } from "./write-header.js";
import { hidKeysHeader, HID_KEYS_HEADER_PATH } from "../../host/src/bake/headers.js";

writeHeader(HID_KEYS_HEADER_PATH, hidKeysHeader());
