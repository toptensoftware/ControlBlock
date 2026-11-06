#!/usr/bin/env node
// node tools/codegen/gen-types.js
// Writes firmware/main/config_types.gen.h from the bake's binpack schema (host/src/bake/schema.js).
// The text comes from host/src/bake/headers.js; host's test/bake/types-header.test.js fails if the
// committed file is out of date.

import { writeHeader } from "./write-header.js";
import { typesHeader, TYPES_HEADER_PATH } from "../../host/src/bake/headers.js";

writeHeader(TYPES_HEADER_PATH, typesHeader());
