// Maps a workgroup's linear index to a block of C and a slice of K, sweeping
// kRasterRows block rows at a time, so workgroups that run together share rows of
// A and columns of B in the GPU's caches instead of streaming whole panels of one
// of them. The dispatch is (slices x block rows x block columns, 1, batch
// items), one slice's blocks after another. Returns the block's (column, row,
// slice).
layout(constant_id = 5) const uint kRasterRows = 8;

uvec3 raster_block(uint blocks_m, uint blocks_n) {
  uint slice = gl_WorkGroupID.x / (blocks_m * blocks_n);
  uint id = gl_WorkGroupID.x - slice * blocks_m * blocks_n;
  uint group = id / (kRasterRows * blocks_n);
  uint first = group * kRasterRows;
  uint rows = min(kRasterRows, blocks_m - first);
  uint offset = id - group * kRasterRows * blocks_n;
  return uvec3(offset / rows, first + offset % rows, slice);
}
