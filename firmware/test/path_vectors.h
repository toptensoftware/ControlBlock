#pragma once

// The render lab's path test vectors, data/path_vectors.bin (format in
// tools/render-lab/path-format.mjs; written by gen-path-vectors.mjs): paths
// with the JS reference rasteriser's mask for each. Read with the lab's
// reader (render_lab_cases.h) in TestPathRaster.cpp.

#include <stddef.h>
#include <stdint.h>
#include "Core.h"
#include "render_lab_cases.h"

typedef RenderLabVector PathVector;

// The file's bytes, read once
const void *PathVectorFile(size_t *size);

// Every vector, read once
const SimpleLib::List<PathVector> &PathVectors();

// The vector called `name` (asserts it exists)
const PathVector &FindPathVector(const char *name);
