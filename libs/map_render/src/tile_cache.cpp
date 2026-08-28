// SPDX-License-Identifier: GPL-3.0-or-later

#include "map_render/tile_cache.h"

#include <algorithm>
#include <string>
#include <variant>

namespace map_render
{

std::size_t approximateBytes(const CachedTile& tile)
{
    std::size_t bytes = sizeof(CachedTile);

    if (tile.geometry)
    {
        bytes += sizeof(TileGeometry);
        bytes += tile.geometry->vertices.capacity() * sizeof(MapVertex);
    }

    if (tile.labels)
    {
        bytes += sizeof(LabelSet);
        bytes += tile.labels->labels.capacity() * sizeof(LabelCandidate);
        // The runs the road labels are drawn along. Counted because a dense
        // tile's roads carry far more geometry than their names do, and a
        // budget that missed it would hold hundreds of tiles it had sized as
        // if they were only text.
        bytes += tile.labels->path.capacity() * sizeof(LocalPoint);
        for (const LabelCandidate& candidate : tile.labels->labels)
        {
            // QString stores UTF-16; capacity is in characters.
            bytes += std::size_t(candidate.text.capacity()) * sizeof(QChar);
        }
    }

    return bytes;
}

void TileCache::touch(const TileId& id)
{
    const auto at = mAt.find(id);
    if (at == mAt.end())
    {
        return;
    }
    // splice, not erase-and-insert: the iterator stays valid, so the index does
    // not have to be rewritten.
    mOrder.splice(mOrder.end(), mOrder, at->second);
}

void TileCache::insert(const TileId& id, CachedTile tile)
{
    tile.bytes = approximateBytes(tile);
    const std::size_t arrived = tile.bytes;

    // What the entry being replaced weighed, read BEFORE it is overwritten.
    // Without this the running total only ever grows, and the byte bound
    // eventually evicts everything down to the floor on every insert.
    const auto existing = mEntries.find(id);
    if (existing != mEntries.end())
    {
        mBytes -= std::min(mBytes, existing->second.bytes);
        existing->second = std::move(tile);
        mBytes += arrived;
        touch(id);
        evictIfNeeded();
        return;
    }

    mEntries.emplace(id, std::move(tile));
    mOrder.push_back(id);
    mAt[id] = std::prev(mOrder.end());
    mBytes += arrived;
    evictIfNeeded();
}

const CachedTile* TileCache::find(const TileId& id)
{
    const auto found = mEntries.find(id);
    if (found == mEntries.end())
    {
        return nullptr;
    }
    touch(id);
    return &found->second;
}

bool TileCache::drawable(const TileId& id)
{
    const CachedTile* tile = find(id);
    // INDICES, not vertices: since the geometry is indexed, the indices are
    // what a draw call consumes. A tile with vertices and no indices draws
    // nothing, and offering it as a stand-in would spend a draw slot on it.
    return tile != nullptr && tile->geometry && !tile->geometry->indices.empty();
}

const TileId* TileCache::nextEviction() const
{
    return mOrder.empty() ? nullptr : &mOrder.front();
}

void TileCache::evictIfNeeded()
{
    // Two bounds, and the byte one yields to the floor. Evicting a tile the
    // paint pass is still drawing re-requests it on the next paint, which is a
    // loop rather than a saving -- see kMinTiles.
    const auto overBudget = [this] {
        if (mEntries.size() > kMaxTiles)
        {
            return true;
        }
        return mBytes > kMaxBytes && mEntries.size() > kMinTiles;
    };

    while (overBudget() && !mOrder.empty())
    {
        const TileId oldest = mOrder.front();
        if (const auto found = mEntries.find(oldest); found != mEntries.end())
        {
            mBytes -= std::min(mBytes, found->second.bytes);
            mEntries.erase(found);
        }
        mOrder.pop_front();
        mAt.erase(oldest);
    }
}

void TileCache::clear()
{
    mEntries.clear();
    mOrder.clear();
    mAt.clear();
    mBytes = 0;
}

} // namespace map_render
