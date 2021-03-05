// Copyright (c) 2018
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Kazys Stepanas
#include "OhmTestConfig.h"

#include <gtest/gtest.h>

#include <ohm/MapLayer.h>
#include <ohm/MapLayout.h>
#include <ohm/OccupancyMap.h>
#include <ohm/VoxelBlock.h>
#include <ohm/VoxelBlockCompressionQueue.h>

#include <ohmutil/OhmUtil.h>

TEST(Compression, Managed)
{
  ohm::VoxelBlockCompressionQueue compressor(true);  // Instantiate in test mode
  // Create a map in order to use the layout. DO NOT SET kCompressed. That would start a new compression object.
  ohm::OccupancyMap map(1.0, ohm::MapFlag::kNone);
  std::vector<ohm::VoxelBlock::Ptr> blocks;
  std::vector<uint8_t> compression_buffer;

  const size_t block_count = 10;
  // Create a set of blocks which we can register with the compression object.
  const ohm::MapLayer &layer = map.layout().layer(map.layout().occupancyLayer());
  const size_t layer_mem_size = layer.layerByteSize(map.regionVoxelDimensions());
  size_t uncompressed_size = layer_mem_size * block_count;
  for (size_t i = 0; i < block_count; ++i)
  {
    blocks.emplace_back();
    blocks[i].reset(new ohm::VoxelBlock(map.detail(), layer));
    compressor.push(blocks[i].get());
  }
  // Set the high water mark above the current allocation size.
  compressor.setHighWaterMark((block_count + 1) * layer_mem_size);
  compressor.__tick(compression_buffer);

  // No compression should have occurred.
  std::cout << "allocated: " << ohm::util::Bytes(compressor.estimatedAllocationSize()) << std::endl;
  EXPECT_EQ(compressor.estimatedAllocationSize(), uncompressed_size);

  // Now lock all the buffers and set a zero high/low water mark. Everything should stay allocated.
  for (auto &block : blocks)
  {
    block->retain();
  }
  compressor.setHighWaterMark(0);
  compressor.setLowWaterMark(0);
  compressor.__tick(compression_buffer);
  EXPECT_EQ(compressor.estimatedAllocationSize(), uncompressed_size);

  // Now unlock and compress everything.
  for (auto &block : blocks)
  {
    block->release();
  }
  compressor.__tick(compression_buffer);
  const size_t compressed_size = compressor.estimatedAllocationSize();
  // All should compress the same.
  const size_t single_block_compressed_size = compressed_size / block_count;
  std::cout << "compressed: " << ohm::util::Bytes(compressed_size) << std::endl;
  std::cout << "single-block-compressed: " << ohm::util::Bytes(single_block_compressed_size) << std::endl;
  EXPECT_LT(compressor.estimatedAllocationSize(), uncompressed_size);
  for (auto &block : blocks)
  {
    EXPECT_FALSE((block->flags() & ohm::VoxelBlock::kFUncompressed));
    EXPECT_FALSE((block->flags() & ohm::VoxelBlock::kFLocked));
  }

  // Relock everything.
  for (auto &block : blocks)
  {
    block->retain();
    EXPECT_TRUE((block->flags() & ohm::VoxelBlock::kFUncompressed));
  }
  // Expect everything uncompressed again. Note we only run the tick to update the allocation size.
  compressor.__tick(compression_buffer);
  EXPECT_EQ(compressor.estimatedAllocationSize(), uncompressed_size);

  // Now leave the high water mark at zero, but raise the low water mark to keep everything allocated. We'll then lower
  // the low water mark and expect one block at a time to be released.
  // This may not scale with increased block_count as compressed size isn't zero.
  compressor.setLowWaterMark(uncompressed_size + 1);
  for (auto &block : blocks)
  {
    block->release();
    EXPECT_TRUE((block->flags() & ohm::VoxelBlock::kFUncompressed));
  }
  compressor.__tick(compression_buffer);
  EXPECT_EQ(compressor.estimatedAllocationSize(), uncompressed_size);

  auto count_uncompressed_blocks = [](const std::vector<ohm::VoxelBlock::Ptr> &blocks) -> size_t {
    size_t count = 0;
    for (const auto &block : blocks)
    {
      if (block->flags() & ohm::VoxelBlock::kFUncompressed)
      {
        ++count;
      }
    }
    return count;
  };

  for (size_t i = block_count; i > 0; --i)
  {
    // Lower water mark.
    compressor.setLowWaterMark(layer_mem_size * i);
    compressor.__tick(compression_buffer);
    const size_t uncompressed_count = count_uncompressed_blocks(blocks);
    EXPECT_EQ(uncompressed_count, i - 1);
    EXPECT_LT(compressor.estimatedAllocationSize(), layer_mem_size * i);
    EXPECT_GE(compressor.estimatedAllocationSize(), layer_mem_size * uncompressed_count);
  }

  // Ensure the blocks are releases.
  blocks.clear();
  compressor.__tick(compression_buffer);
}
