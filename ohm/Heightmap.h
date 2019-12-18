// Copyright (c) 2018
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Kazys Stepanas
#ifndef HEIGHTMAP_H
#define HEIGHTMAP_H

#include "OhmConfig.h"

#include "Aabb.h"
#include "UpAxis.h"

#include <memory>

#include <glm/fwd.hpp>

#include <vector>

namespace ohm
{
  struct HeightmapDetail;
  class Key;
  class MapInfo;
  class OccupancyMap;
  class VoxelConst;

  /// A 2D voxel map variant which calculate a heightmap surface from another @c OccupancyMap.
  ///
  /// The heightmap is built from an @c OccupancyMap and forms an axis aligned collapse of that map. The up axis may be
  /// specified on construction of the heightmap, but must be aligned to a primary axis. The heightmap is build in
  /// its own @c OccupancyMap, which consists of a single layer of voxels. The @c MapLayout for the heightmap is
  /// two layers:
  /// - **occupancy** layer
  ///   - float occupancy
  /// - *heightmap* layer (named from @c HeightmapVoxel::kHeightmapLayer)
  ///   - @c HeightmapVoxel
  ///
  /// The height specifies the absolute height of the surface, while clearance denotes how much room there is above
  /// the surface voxel before the next obstruction. Note that the height values always increase going up, so the
  /// height value will be inverted when using any @c UpAxis::kNegN @c UpAxis value. Similarly, the clearance is always
  /// positive unless there are no further voxels above the surface, in which case the clearance is zero
  /// (no information).
  ///
  /// Each voxel in the heightmap represents a collapse of the source @c OccupancyMap based on a seed reference
  /// position - see @c buildHeightmap(). The heightmap is generated by considering each column in the source map
  /// relative to a reference height based on the seed position and neighbouring cells. When a valid supporting surface
  /// is found, a heightmap voxel is marked as occupied and given a height associated with the supporting surface. This
  /// supporting surface is the closest occupied voxel to the current reference position also having sufficient
  /// clearance above it, @c minClearance().
  ///
  /// The heightmap may also generate a 'virtual surface' from the interface between uncertain and free voxels when
  /// @c generateVirtualSurface() is set. A 'virtual surface' voxel is simply a free voxel with an uncertain voxel below
  /// it, but only in a column which does not have an occupied voxel within the search range. Virtual surface voxels
  /// are marked as free in the heightmap.
  ///
  /// The heightmap is generated either using a planar seach or a flood fill from the reference position. The planar
  /// search operates at a fixed reference height at each column, while the flood fill search height is dependent on
  /// the height of neighbour voxels. The flood fill is better at following surfaces, however it is significantly
  /// slower.
  ///
  /// Some variables limit the search for a supporting voxel in each column. To be considered as a support candidate, a
  /// voxel must;
  ///
  /// - Lie within the extents given to @c buildHeightmap()
  /// - Must not be higher than the @c ceiling() height above its starting search position.
  ///
  /// The generated heightmap may be accessed via @c heightmap() and voxel positions should be retrieved using
  /// @c getHeightmapVoxelPosition() .
  ///
  /// Finally, the heightmap supports a local cache, to preseve features around the @c reference_position given to
  /// @c buildHeightmap() . This is intended to deal with potential blind spots and occupancy map erosion issues.
  /// When enabled, the cache is used for each column where a real surface cannot be generated (virtual surface or
  /// uncertain). The cache is regenerated after the heightmap is generated. The cache must be seeded to enable it's
  /// use by calling @c seedLocalCache(). The cache is accessible via @c heightmapLocalCache(). The cache represents
  /// a region around a vehicle or sensor and requires a reference position.
  ///
  /// The @c OccupancyMap used to represent the heightmap has additional meta data stored in it's @c MapInfo:
  /// - <b>heightmap</b> - Present and true if this is a heightmap.
  /// - <b>heightmap-axis</b> - The up axis ID for a heightmap.
  /// - <b>heightmap-axis-x</b> - The up axis X value for a heightmap.
  /// - <b>heightmap-axis-y</b> - The up axis Y value for a heightmap.
  /// - <b>heightmap-axis-z</b> - The up axis Z value for a heightmap.
  /// - <b>heightmap-blur</b> - The blur value used to generate the heightamp.
  /// - <b>heightmap-clearance</b> - The clearance value used to generate the heightamp.
  class Heightmap
  {
  public:
    /// Size of regions in the heightmap. This is a 2D voxel extent. The region height is always one voxel.
    static const unsigned kDefaultRegionSize = 128;
    /// Voxel value assigned to heightmap cells which represent a real surface extracted from the source map.
    static constexpr float kHeightmapSurfaceValue = 1.0f;
    /// Voxel value assigned to heightmap cells which represent a virtual surface extracted from the source map.
    /// Virtual surfaces may be formed by the interface between a free voxel supported by an uncertain/null voxel.
    static constexpr float kHeightmapVirtualSurfaceValue = -1.0f;
    /// Voxel value assigned to heightmap cells which have no valid voxel in the entire column from the source map.
    static constexpr float kHeightmapVacantValue = 0.0f;

    /// Construct a default initialised heightmap.
    Heightmap();

    /// Construct a new heightmap optionally tied to a specific @p map.
    /// @param grid_resolution The grid resolution for the heightmap. Should match the source map for best results.
    /// @param min_clearance The minimum clearance value expected above each surface voxel.
    /// @param up_axis Identifies the up axis for the map.
    /// @param region_size Grid size of each region in the heightmap.
    Heightmap(double grid_resolution, double min_clearance, UpAxis up_axis = UpAxis::kZ, unsigned region_size = 0);

    /// Destructor.
    ~Heightmap();

    /// Set number of threads to use in heightmap generation, enabling multi-threaded code path as required.
    ///
    /// Setting the @p thread_count to zero enabled multi-threading using the maximum number of threads. Setting the
    /// @p thread_count to 1 disables threads (default).
    ///
    /// Using multiple threads may not yield significant gains.
    ///
    /// @param thread_count The number of threads to set.
    /// @return True if mult-threading is available. False when no mult-threading is available and @p thread_count is
    /// ignored.
    bool setThreadCount(unsigned thread_count);

    /// Get the number of threads to use.
    ///
    /// - 0: use all available
    /// - 1: force single threaded, or no multi-threading is available.
    /// - n: Use n threads.
    /// @return The number of threads to use.
    unsigned threadCount() const;

    /// Set the occupancy map on which to base the heightmap. The heightmap does not take ownership of the pointer so
    /// the @p map must persist until @c buildHeightmap() is called.
    void setOccupancyMap(OccupancyMap *map);

    /// Access the current source occupancy map.
    OccupancyMap *occupancyMap() const;

    /// Access the currently generated heightmap.
    OccupancyMap &heightmap() const;

    /// Access the cache of the heightmap around the last reference position.
    OccupancyMap *heightmapLocalCache() const;

    /// Set the ceiling level. Points above this distance above the base height in the source map are ignored.
    /// @param ceiling The new ceiling value. Positive to enable.
    void setCeiling(double ceiling);

    /// Get the ceiling level. Points above this distance above the base height in the source map are ignored.
    /// @return The ceiling value.
    double ceiling() const;

    /// Set the minimum clearance required above a voxel in order to consider it a heightmap voxel.
    /// @param clearance The new clearance value.
    void setMinClearance(double clearance);

    /// Get the minimum clearance required above a voxel in order to consider it a heightmap voxel.
    /// @return The height clearance value.
    double minClearance() const;

    /// Sets whether sub-voxel positions are ignored (true) forcing the use of voxel centres.
    /// @param ignore True to force voxel centres even when sub-voxel positions are present.
    void setIgnoreSubVoxelPositioning(bool ignore);

    /// Force voxel centres even when sub-voxel positions are present?
    /// @return True to ignore sub-voxel positioning.
    /// @seealso @ref subvoxel
    bool ignoreSubVoxelPositioning() const;

    /// Set the generation of a heightmap floor around the transition from unknown to free voxels?
    ///
    /// This option allows a heightmap floor to be generated in columns where there is no clear occupied floor voxel.
    /// When enabled, the heightmap generates a floor level at the lowest transition point from unknown to free voxel.
    ///
    /// @param enable Enable this option?
    void setGenerateVirtualSurface(bool enable);

    /// Allow the generation of a heightmap floor around the transition from unknown to free voxels?
    ///
    /// @see @c setGenerateVirtualSurface()
    ///
    /// @retrun True if this option is enabled.
    bool generateVirtualSurface() const;

    /// Set the heightmap generation to flood fill (@c true) or planar (@c false).
    /// @param flood_fill True to enable the flood fill technique.
    void setUseFloodFill(bool flood_fill);

    /// Is the flood fill generation technique in use (@c true) or planar technique (@c false).
    /// @return True when using flood fill.
    bool useFloodFill() const;

    /// Set the size of the @c heightmapLocalCache() .
    /// @param extents The cache extents (axis aligned).
    void setLocalCacheExtents(double extents);

    /// Get the size of the @c heightmapLocalCache() .
    /// @return The cache extents (axis aligned).
    double localCacheExtents() const;

    /// The layer number which contains @c HeightmapVoxel structures.
    /// @return The heightmap layer index or -1 on error (not present).
    /// @seealso @ref subvoxel
    int heightmapVoxelLayer() const;

    /// The layer number which contains @c HeightmapVoxel structures during heightmap construction.
    /// @return The heightmap build layer index or -1 on error (not present).
    int heightmapVoxelBuildLayer() const;

    /// Get the up axis identifier used to generate the heightmap.
    UpAxis upAxis() const;

    /// Get the up axis index [0, 2] marking XYZ respectively. Ignores direction.
    int upAxisIndex() const;

    /// Get the normal vector for the up axis used to last @c buildHeightmap().
    const glm::dvec3 &upAxisNormal() const;

    /// Component index of the first surface axis normal [0, 2].
    int surfaceAxisIndexA() const;

    /// Get a unit vector which lies along the surface of the heightmap, perpendicular to @c surfaceAxisB() and
    /// upAxisNormal().
    const glm::dvec3 &surfaceAxisA() const;

    /// Component of the second surface axis normal [0, 2].
    int surfaceAxisIndexB() const;

    /// Get a unit vector which lies along the surface of the heightmap, perpendicular to @c surfaceAxisA() and
    /// upAxisNormal().
    const glm::dvec3 &surfaceAxisB() const;

    /// Static resolution of @c Axis to a normal.
    /// @param id The @c Axis ID.
    static const glm::dvec3 &upAxisNormal(UpAxis axis_id);

    /// Get a unit vector which lies along the surface of the heightmap, perpendicular to @c surfaceAxisB() and
    /// upAxisNormal().
    static const glm::dvec3 &surfaceAxisA(UpAxis axis_id);

    /// Get a unit vector which lies along the surface of the heightmap, perpendicular to @c surfaceAxisA() and
    /// upAxisNormal().
    static const glm::dvec3 &surfaceAxisB(UpAxis axis_id);

    /// Seed and enable the local cache (see class documentation).
    /// @param reference_pos The position around which to seed the local cache.
    void seedLocalCache(const glm::dvec3 &reference_pos);

    /// Generate the heightmap around a reference position. This sets the @c base_height as in the overload, but also
    /// changes the behaviour to flood fill out from the reference position.
    ///
    /// @param reference_pos The staring position to build a heightmap around. Nominally a vehicle or sensor position.
    /// @return true on success.
    bool buildHeightmap(const glm::dvec3 &reference_pos, const ohm::Aabb &cull_to = ohm::Aabb(0.0));

    /// Query the position of a voxel in the @c heightmap() occupancy map. This method also supports voxels from
    /// @c heightmapLocalCache().
    ///
    /// Heightmap voxel values, positions and semantics are specialised from the general @c OccupancyMap usage. This
    /// method may be used to ensure the correct position values are retrieved and consistent voxel interpretations
    /// are made. All position queries should be made via this function. The return value is used indicate whether
    /// the voxel is relevant/occupied within the occupancy map.
    ///
    /// This overload accepts a @p reference_position which nominally indicates the position of a vehicle navigating
    /// the heightmap. This position is used to help with the identification of negative obstacles (holes and drops).
    ///
    /// The @p negative_obstacle_radius identifies a 2D range from the @p reference_position within which voxels may be
    /// considered as negative obstacles. Voxels cannot be adequately resolved from the source map into the heightmap
    /// may represent negative obstacles. Such voxels, either uncertain or representing virtual surfaces, falling within
    /// this radius report a height designed to generate a parabolic surface. When later costing by slope this leads to
    /// high cost regions.
    ///
    /// @param heightmap_voxel The voxel to test for validity and to retrieve the position of. This voxel must be from
    ///   either the @p heightmap() or @p heightmapLocalCache() of this object or behaviour is undefined.
    /// @param reference_position A reference position of a vehicle navigating the map. Used to generate negative
    ///     obstacle surfaces.
    /// @param negative_obstacle_radius The 2D range from the @c reference_position within which negative obstacle
    ///     surfaces may be generated.
    /// @param[out] pos The retrieved position of @p heightmap_voxel. Only valid when this function returns @c true.
    /// @param[out] clearance The available height clearance above @p heightmap_voxel. Only valid when this function
    ///     returns @c true.
    /// @return True if @p heightmap_voxel is valid and occupied.
    bool getHeightmapVoxelPosition(const VoxelConst &heightmap_voxel, const glm::dvec3 &reference_position,
                                   double negative_obstacle_radius, glm::dvec3 *pos, float *clearance = nullptr) const;

    /// @overload
    bool getHeightmapVoxelPosition(const VoxelConst &heightmap_voxel, glm::dvec3 *pos,
                                   float *clearance = nullptr) const;

    //-------------------------------------------------------
    // Internal
    //-------------------------------------------------------
    /// @internal
    inline HeightmapDetail *detail() { return imp_.get(); }
    /// @internal
    inline const HeightmapDetail *detail() const { return imp_.get(); }

    /// Update @c info to reflect the details of how the heightmap is generated. See class comments.
    /// @param info The info object to update.
    void updateMapInfo(MapInfo &info) const;  // NOLINT(google-runtime-references)

    /// Ensure that @p key is referencing a voxel within the heightmap plane.
    /// @param key[in,out] The key to project. May be modified by this call. Must not be null.
    /// @return A reference to @p key.
    Key &project(Key *key);

  private:
    /// Update the local cache from the current heightmap.
    /// @param reference_pos The position around which to generate the cache.
    void updateLocalCache(const glm::dvec3 &reference_pos);

    /// Query the local cache.
    /// @param lookup_pos The source position to query. Will be flattened to 2D.
    /// @param[out] cache_pos The position value retrieved from the cache on success.
    /// @param[out] cache_value The voxel value of the cache voxel.
    /// @param[out] clearance The overhead clearance value of the cache voxel.
    /// @return True when @p lookup_pos has resolved into a valid cache voxel.
    bool lookupLocalCache(const glm::dvec3 &lookup_pos, glm::dvec3 *cache_pos, float *cache_value, double *clearance);

    /// @internal
    /// Internal implementation of heightmap construction. Supports the different key walking techniques available.
    /// @param walker The key walker used to iterate the source map and heightmap overlap.
    /// @param reference_pos Reference position around which to generate the heightmap
    /// @param on_visit Optional callback invoked for each key visited. Parameters are: @p walker, this object's
    ///   internal details, the candidate key first evaluated for the column search start, the ground key to be migrated
    ///   to the heightmap. Both keys reference the source map.
    template <typename KeyWalker>
    bool buildHeightmapT(KeyWalker &walker, const glm::dvec3 &reference_pos,
                         void (*on_visit)(KeyWalker &, const HeightmapDetail &, const Key &, const Key &) = nullptr);

    std::unique_ptr<HeightmapDetail> imp_;
  };
}  // namespace ohm

#endif  // HEIGHTMAP_H
