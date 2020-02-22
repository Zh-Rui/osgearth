/* -*-c++-*- */
/* osgEarth - Geospatial SDK for OpenSceneGraph
 * Copyright 2020 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <osgEarth/ElevationLayer>
#include <osgEarth/HeightFieldUtils>
#include <osgEarth/Progress>
#include <osgEarth/MemCache>
#include <osgEarth/Metrics>

using namespace osgEarth;
using namespace OpenThreads;

#define LC "[ElevationLayer] \"" << getName() << "\" : "

//#define ANALYZE

//------------------------------------------------------------------------

Config
ElevationLayer::Options::getConfig() const
{
    Config conf = TileLayer::Options::getConfig();
    conf.set("vdatum", verticalDatum() );
    conf.set("offset", offset());
    conf.set("nodata_policy", "default",     _noDataPolicy, NODATA_INTERPOLATE );
    conf.set("nodata_policy", "interpolate", _noDataPolicy, NODATA_INTERPOLATE );
    conf.set("nodata_policy", "msl",         _noDataPolicy, NODATA_MSL );
    return conf;
}

void
ElevationLayer::Options::fromConfig( const Config& conf )
{
    _offset.init( false );
    _noDataPolicy.init( NODATA_INTERPOLATE );
    
    conf.get("vdatum", verticalDatum() );
    conf.get("vsrs", verticalDatum() );    // back compat
    conf.get("offset", offset() );
    conf.get("nodata_policy", "default",     _noDataPolicy, NODATA_INTERPOLATE );
    conf.get("nodata_policy", "interpolate", _noDataPolicy, NODATA_INTERPOLATE );
    conf.get("nodata_policy", "msl",         _noDataPolicy, NODATA_MSL );
}

//------------------------------------------------------------------------

namespace
{
    // perform very basic sanity-check validation on a heightfield.
    bool validateHeightField(osg::HeightField* hf)
    {
        if (!hf) 
            return false;
        if (hf->getNumRows() < 2 || hf->getNumRows() > 1024) {
            OE_WARN << "row count = " << hf->getNumRows() << std::endl;
            return false;
        }
        if (hf->getNumColumns() < 2 || hf->getNumColumns() > 1024) {
            OE_WARN << "col count = " << hf->getNumColumns() << std::endl;
            return false;
        }
        if (hf->getHeightList().size() != hf->getNumColumns() * hf->getNumRows()) {
            OE_WARN << "mismatched data size" << std::endl;
            return false;
        }
        //if (hf->getXInterval() < 1e-5 || hf->getYInterval() < 1e-5)
        //    return false;
        
        return true;
    }    
}

//------------------------------------------------------------------------

OE_LAYER_PROPERTY_IMPL(ElevationLayer, std::string, VerticalDatum, verticalDatum);
OE_LAYER_PROPERTY_IMPL(ElevationLayer, bool, Offset, offset);
OE_LAYER_PROPERTY_IMPL(ElevationLayer, ElevationNoDataPolicy, NoDataPolicy, noDataPolicy);

void
ElevationLayer::init()
{
    TileLayer::init();

    // elevation layers do not render directly; rather, a composite of elevation data
    // feeds the terrain engine to permute the mesh.
    setRenderType(RENDERTYPE_NONE);

    // sync up enabled & visible
    if (getVisible() != getEnabled())
        setVisible(getEnabled());
}

void
ElevationLayer::setVisible(bool value)
{
    VisibleLayer::setVisible(value);
    setEnabled(value);
}

void
ElevationLayer::setEnabled(bool value)
{
    VisibleLayer::setVisible(value);
    VisibleLayer::setEnabled(value);
}

bool
ElevationLayer::isOffset() const
{
    return options().offset().get();
}

void
ElevationLayer::normalizeNoDataValues(osg::HeightField* hf) const
{
    if ( hf )
    {
        osg::FloatArray* values = hf->getFloatArray();
        for(osg::FloatArray::iterator i = values->begin(); i != values->end(); ++i)
        {
            float& value = *i;
            if ( osg::isNaN(value) || osg::equivalent(value, getNoDataValue()) || value < getMinValidValue() || value > getMaxValidValue() )
            {
                OE_DEBUG << "Replaced " << value << " with NO_DATA_VALUE" << std::endl;
                value = NO_DATA_VALUE;
            }
        } 
    }
}

void
ElevationLayer::applyProfileOverrides()
{
    // Check for a vertical datum override.
    bool changed = false;
    if ( getProfile() && options().verticalDatum().isSet() )
    {
        std::string vdatum = options().verticalDatum().get();
        OE_INFO << LC << "Override vdatum = " << vdatum << ", profile vdatum = " << _profile->getSRS()->getVertInitString() << std::endl;
        if ( !ciEquals(getProfile()->getSRS()->getVertInitString(), vdatum) )
        {
            ProfileOptions po = getProfile()->toProfileOptions();
            po.vsrsString() = vdatum;
            setProfile( Profile::create(po) );
            changed = true;
        }
    }

    if (changed && _profile.valid())
    {
        OE_INFO << LC << "Override profile: " << _profile->toString() << std::endl;
    }
}

void
ElevationLayer::assembleHeightField(const TileKey& key,
                                    osg::ref_ptr<osg::HeightField>& out_hf,
                                    osg::ref_ptr<NormalMap>& out_normalMap,
                                    ProgressCallback* progress) const
{			
    OE_PROFILING_ZONE;

    // Collect the heightfields for each of the intersecting tiles.
    GeoHeightFieldVector heightFields;

    //Determine the intersecting keys
    std::vector< TileKey > intersectingTiles;
    
    if (key.getLOD() > 0u)
    {
        getProfile()->getIntersectingTiles(key, intersectingTiles);
    }

    else
    {
        // LOD is zero - check whether the LOD mapping went out of range, and if so,
        // fall back until we get valid tiles. This can happen when you have two
        // profiles with very different tile schemes, and the "equivalent LOD" 
        // surpasses the max data LOD of the tile source.
        unsigned numTilesThatMayHaveData = 0u;

        int intersectionLOD = getProfile()->getEquivalentLOD(key.getProfile(), key.getLOD());

        while (numTilesThatMayHaveData == 0u && intersectionLOD >= 0)
        {
            intersectingTiles.clear();
            getProfile()->getIntersectingTiles(key.getExtent(), intersectionLOD, intersectingTiles);

            for (unsigned int i = 0; i < intersectingTiles.size(); ++i)
            {
                const TileKey& layerKey = intersectingTiles[i];
                if (mayHaveData(layerKey) == true)
                {
                    ++numTilesThatMayHaveData;
                }
            }

            --intersectionLOD;
        }
    }

    // collect heightfield for each intersecting key. Note, we're hitting the
    // underlying tile source here, so there's no vetical datum shifts happening yet.
    // we will do that later.
    if ( intersectingTiles.size() > 0 )
    {
        for (unsigned int i = 0; i < intersectingTiles.size(); ++i)
        {
            const TileKey& layerKey = intersectingTiles[i];

            if ( isKeyInLegalRange(layerKey) )
            {
                GeoHeightField hf = createHeightFieldImplementation(layerKey, progress);
                if (hf.valid())
                {
                    heightFields.push_back( hf );
                }
            }
        }

        // If we actually got a HeightField, resample/reproject it to match the incoming TileKey's extents.
        if (heightFields.size() > 0)
        {		
            unsigned int width = 0;
            unsigned int height = 0;

            for (GeoHeightFieldVector::iterator itr = heightFields.begin(); itr != heightFields.end(); ++itr)
            {
                if (itr->getHeightField()->getNumColumns() > width)
                    width = itr->getHeightField()->getNumColumns();
                if (itr->getHeightField()->getNumRows() > height) 
                    height = itr->getHeightField()->getNumRows();                        
            }

            //Now sort the heightfields by resolution to make sure we're sampling the highest resolution one first.
            std::sort( heightFields.begin(), heightFields.end(), GeoHeightField::SortByResolutionFunctor());        

            out_hf = new osg::HeightField();
            out_hf->allocate(width, height);

            out_normalMap = new NormalMap(width, height);

            //Go ahead and set up the heightfield so we don't have to worry about it later
            double minx, miny, maxx, maxy;
            key.getExtent().getBounds(minx, miny, maxx, maxy);
            double dx = (maxx - minx)/(double)(width-1);
            double dy = (maxy - miny)/(double)(height-1);

            //Create the new heightfield by sampling all of them.
            for (unsigned int c = 0; c < width; ++c)
            {
                double x = minx + (dx * (double)c);
                for (unsigned r = 0; r < height; ++r)
                {
                    double y = miny + (dy * (double)r);

                    //For each sample point, try each heightfield.  The first one with a valid elevation wins.
                    float elevation = NO_DATA_VALUE;
                    osg::Vec3 normal(0,0,1);

                    for (GeoHeightFieldVector::iterator itr = heightFields.begin(); itr != heightFields.end(); ++itr)
                    {
                        // get the elevation value, at the same time transforming it vertically into the 
                        // requesting key's vertical datum.
                        float e = 0.0;
                        osg::Vec3 n;
                        if (itr->getElevationAndNormal(key.getExtent().getSRS(), x, y, INTERP_BILINEAR, key.getExtent().getSRS(), e, n))
                        {
                            elevation = e;
                            normal = n;
                            break;
                        }
                    }
                    out_hf->setHeight( c, r, elevation );   
                    out_normalMap->set( c, r, normal );
                }
            }
        }
        else
        {
            //if (progress && progress->message().empty())
            //    progress->message() = "assemble yielded no heightfields";
        }
    }
    else
    {
        //if (progress && progress->message().empty())
        //    progress->message() = "assemble yielded no intersecting tiles";
    }


    // If the progress was cancelled clear out any of the output data.
    if (progress && progress->isCanceled())
    {
        out_hf = 0;
        out_normalMap = 0;
    }
}

GeoHeightField
ElevationLayer::createHeightField(const TileKey& key)
{
    return createHeightField(key, 0L);
}

GeoHeightField
ElevationLayer::createHeightField(const TileKey& key, ProgressCallback* progress)
{
    OE_PROFILING_ZONE;
    OE_PROFILING_ZONE_TEXT(getName());
    OE_PROFILING_ZONE_TEXT(key.str());

    if (getStatus().isError())
    {
        return GeoHeightField::INVALID;
    }

    // If the layer is disabled, bail out
    if (getEnabled() == false)
    {
        return GeoHeightField::INVALID;
    }

    // prevents 2 threads from creating the same object at the same time
    //_sentry.lock(key);

    GeoHeightField result = createHeightFieldInKeyProfile(key, progress);

    //_sentry.unlock(key);

    return result;
}

GeoHeightField
ElevationLayer::createHeightFieldInKeyProfile(const TileKey& key, ProgressCallback* progress)
{
    GeoHeightField result;
    osg::ref_ptr<osg::HeightField> hf;

    // Check the memory cache first
    bool fromMemCache = false;

    // cache key combines the key with the full signature (incl vdatum)
    // the cache key combines the Key and the horizontal profile.
    std::string cacheKey = Cache::makeCacheKey(
        Stringify() << key.str() << "-" << key.getProfile()->getHorizSignature(),
        "elevation");
    const CachePolicy& policy = getCacheSettings()->cachePolicy().get();

    char memCacheKey[64];

    // Try the L2 memory cache first:
    if ( _memCache.valid() )
    {
        sprintf(memCacheKey, "%d/%s/%s", getRevision(), key.str().c_str(), key.getProfile()->getHorizSignature().c_str());

        CacheBin* bin = _memCache->getOrCreateDefaultBin();
        ReadResult cacheResult = bin->readObject(memCacheKey, 0L);
        if ( cacheResult.succeeded() )
        {
            result = GeoHeightField(
                static_cast<osg::HeightField*>(cacheResult.releaseObject()),
                key.getExtent());

            fromMemCache = true;
        }
    }

    // Next try the main cache:
    if ( !result.valid() )
    {
        // See if there's a persistent cache.
        CacheBin* cacheBin = getCacheBin( key.getProfile() );

        // validate the existance of a valid layer profile.
        if ( !policy.isCacheOnly() && !getProfile() )
        {
            disable("Could not establish a valid profile.. did you set one?");
            return GeoHeightField::INVALID;
        }

        // Now attempt to read from the cache. Since the cached data is stored in the
        // map profile, we can try this first.
        bool fromCache = false;

        osg::ref_ptr< osg::HeightField > cachedHF;

        if ( cacheBin && policy.isCacheReadable() )
        {
            ReadResult r = cacheBin->readObject(cacheKey, 0L);
            if ( r.succeeded() )
            {            
                bool expired = policy.isExpired(r.lastModifiedTime());
                cachedHF = r.get<osg::HeightField>();
                if ( cachedHF && validateHeightField(cachedHF.get()) )
                {
                    if (!expired)
                    {
                        hf = cachedHF;
                        fromCache = true;
                    }
                }
            }
        }

        // if we're cache-only, but didn't get data from the cache, fail silently.
        if ( !hf.valid() && policy.isCacheOnly() )
        {
            return GeoHeightField::INVALID;
        }

        // If we didn't get anything from cache, time to create one:
        if ( !hf.valid() )
        {
            // Check that the key is legal (in valid LOD range, etc.)
            if ( !isKeyInLegalRange(key) )
            {
                return GeoHeightField::INVALID;
            }

            //TODO:
            //getOrCreatePreCacheOp() is only used in TileSource-based path atm.
            //We need to include the funcionality in all 

            if (key.getProfile()->isHorizEquivalentTo(getProfile()))
            {
                result = createHeightFieldImplementation(key, progress);
            }
            else
            {
                // If the profiles are different, use a compositing method to assemble the tile.
                osg::ref_ptr<NormalMap> normalMap;
                osg::ref_ptr<osg::HeightField> hf;
                assembleHeightField(key, hf, normalMap, progress);
                result = GeoHeightField(hf.get(), normalMap.get(), key.getExtent());
            }

            // Check for cancelation before writing to a cache
            if (progress && progress->isCanceled())
            {
                return GeoHeightField::INVALID;
            }

            // The const_cast is safe here because we just created the
            // heightfield from scratch...not from a cache.
            // TODO: note, I don't like this -gw
            hf = const_cast<osg::HeightField*>(result.getHeightField());

            // validate it to make sure it's legal.
            if ( hf.valid() && !validateHeightField(hf.get()) )
            {
                OE_WARN << LC << "Generated an illegal heightfield!" << std::endl;
                hf = 0L; // to fall back on cached data if possible.
            }

            // If the result is good, we now have a heightfield but its vertical values
            // are still relative to the source's vertical datum. Convert them.
            if (hf.valid() && !key.getExtent().getSRS()->isVertEquivalentTo(getProfile()->getSRS()))
            {
                OE_PROFILING_ZONE_NAMED("vdatum xform");

                VerticalDatum::transform(
                    getProfile()->getSRS()->getVerticalDatum(),    // from
                    key.getExtent().getSRS()->getVerticalDatum(),  // to
                    key.getExtent(),
                    hf.get() );
            }

            // Pre-caching operation. If there's a TileSource, it runs the precache
            // operator so we don't need to run it here. This is a temporary construct
            // until we get rid of TileSource
            {
                OE_PROFILING_ZONE_NAMED("nodata normalize");
                normalizeNoDataValues(hf.get());
            }

            // If we have a cacheable heightfield, and it didn't come from the cache
            // itself, cache it now.
            if ( hf.valid()    && 
                 cacheBin      && 
                 !fromCache    &&
                 policy.isCacheWriteable() )
            {
                OE_PROFILING_ZONE_NAMED("cache write");
                cacheBin->write(cacheKey, hf.get(), 0L);
            }

            // If we have an expired heightfield from the cache and were not able to create
            // any new data, just return the cached data.
            if (!hf.valid() && cachedHF.valid())
            {
                OE_DEBUG << LC << "Using cached but expired heightfield for " << key.str() << std::endl;
                hf = cachedHF;
            }

            // No luck on any path:
            if ( !hf.valid() )
            {
                return GeoHeightField::INVALID;
            }
        }

        if ( hf.valid() )
        {
            result = GeoHeightField( hf.get(), key.getExtent() );
        }
    }

    // Check for cancelation before writing to a cache:
    if ( progress && progress->isCanceled() )
    {
        return GeoHeightField::INVALID;
    }

    // write to mem cache if needed:
    if ( result.valid() && !fromMemCache && _memCache.valid() )
    {
        CacheBin* bin = _memCache->getOrCreateDefaultBin();
        bin->write(memCacheKey, result.getHeightField(), 0L);
    }

    return result;
}

Status
ElevationLayer::writeHeightField(const TileKey& key, const osg::HeightField* hf, ProgressCallback* progress) const
{
    if (isWritingSupported() && isWritingRequested())
    {
        return writeHeightFieldImplementation(key, hf, progress);
    }
    return Status::ServiceUnavailable;
}

Status
ElevationLayer::writeHeightFieldImplementation(const TileKey& key, const osg::HeightField* hf, ProgressCallback* progress) const
{
    return Status::ServiceUnavailable;
}


//------------------------------------------------------------------------

#undef  LC
#define LC "[ElevationLayers] "

ElevationLayerVector::ElevationLayerVector()
{
    //nop
}


ElevationLayerVector::ElevationLayerVector(const ElevationLayerVector& rhs) :
osg::MixinVector< osg::ref_ptr<ElevationLayer> >( rhs )
{
    //nop
}



namespace
{
    typedef osg::ref_ptr<ElevationLayer>          RefElevationLayer;
    struct LayerData {
        RefElevationLayer layer;
        TileKey key;
        int index;
    };
    //typedef std::pair<RefElevationLayer, TileKey> LayerAndKey;
    typedef std::vector<LayerData>              LayerDataVector;

    //! Gets the normal vector for elevation data at column s, row t.
    osg::Vec3 getNormal(const GeoExtent& extent, const osg::HeightField* hf, int s, int t)
    {
        int w = hf->getNumColumns();
        int h = hf->getNumRows();

        osg::Vec2d res(
            extent.width() / (double)(w-1),
            extent.height() / (double)(h-1));

        float e = hf->getHeight(s, t);

        double dx = res.x(), dy = res.y();

        if (extent.getSRS()->isGeographic())
        {
            double R = extent.getSRS()->getEllipsoid()->getRadiusEquator();
            double mPerDegAtEquator = (2.0 * osg::PI * R) / 360.0;
            dy = dy * mPerDegAtEquator;
            double lat = extent.yMin() + res.y()*(double)t;
            dx = dx * mPerDegAtEquator * cos(osg::DegreesToRadians(lat));
        }
        
        osg::Vec3d west(0, 0, e), east(0, 0, e), south(0, 0, e), north(0, 0, e);

        if (s > 0)     west.set (-dx, 0, hf->getHeight(s-1, t));
        if (s < w - 1) east.set ( dx, 0, hf->getHeight(s+1, t));
        if (t > 0)     south.set(0, -dy, hf->getHeight(s, t-1));
        if (t < h - 1) north.set(0,  dy, hf->getHeight(s, t+1));

        osg::Vec3d normal = (east - west) ^ (north - south);
        return normal;
    }

    //! Creates a normal map for heightfield "hf" and stores it in the
    //! pre-allocated NormalMap.
    //!
    //! "deltaLOD" holds the difference in LODs between the heightfield itself and the LOD
    //! from which the elevation value came. This will be positive when we had to "fall back" on 
    //! lower LOD data to fetch an elevation value. When this happens we need to interpolate
    //! between "real" normals instead of sampling them from the neighboring pixels, otherwise
    //! ugly faceting will occur.
    //!
    //! Unfortunately, it there's an offset layer, this will update the deltaLOD and we will 
    //! get faceting if the real elevation layer is from a lower LOD. The only solution to this
    //! would be to sample the elevation data using a spline function instead of bilinear
    //! interpolation -- but we would need to do that to a separate heightfield (especially for
    //! normals) in order to maintain terrain correlation. Maybe someday.
    void createNormalMap(const GeoExtent& extent, const osg::HeightField* hf, const osg::ShortArray* deltaLOD, NormalMap* normalMap)
    {
        int w = hf->getNumColumns();
        int h = hf->getNumRows();

        for (int t = 0; t < (int)hf->getNumRows(); ++t)
        {
            for (int s = 0; s<(int)hf->getNumColumns(); ++s)
            {
                int step = 1 << (*deltaLOD)[t*h + s];

                osg::Vec3 normal;

                // four corners:
                int s0=s, s1=s, t0=t, t1=t;

                if (step == 1)
                {
                    // Same LOD, simple query
                    normal = getNormal(extent, hf, s, t);
                }
                else
                {
                    int s0 = osg::maximum(s - (s % step), 0);
                    int s1 = (s%step == 0)? s0 : osg::minimum(s0+step, w-1);
                    int t0 = osg::maximum(t - (t % step), 0);
                    int t1 = (t%step == 0)? t0 : osg::minimum(t0+step, h-1);
                    
                    if (s0 == s1 && t0 == t1)
                    {
                        // on-pixel, simple query
                        normal = getNormal(extent, hf, s0, t0);
                    }
                    else if (s0 == s1)
                    {
                        // same column; linear interpolate along row
                        osg::Vec3 S = getNormal(extent, hf, s0, t0);
                        osg::Vec3 N = getNormal(extent, hf, s0, t1);
                        normal = S*(double)(t1 - t) + N*(double)(t - t0);
                    }
                    else if (t0 == t1)
                    {
                        // same row; linear interpolate along column
                        osg::Vec3 W = getNormal(extent, hf, s0, t0);
                        osg::Vec3 E = getNormal(extent, hf, s1, t0);
                        normal = W*(double)(s1 - s) + E*(double)(s - s0);
                    }
                    else
                    {
                        // bilinear interpolate
                        osg::Vec3 SW = getNormal(extent, hf, s0, t0);
                        osg::Vec3 SE = getNormal(extent, hf, s1, t0);
                        osg::Vec3 NW = getNormal(extent, hf, s0, t1);
                        osg::Vec3 NE = getNormal(extent, hf, s1, t1);

                        osg::Vec3 S = SW*(double)(s1 - s) + SE*(double)(s - s0);
                        osg::Vec3 N = NW*(double)(s1 - s) + NE*(double)(s - s0);
                        normal = S*(double)(t1 - t) + N*(double)(t - t0);
                    }
                }

                normal.normalize();

                normalMap->set(s, t, normal, 0.0f);
            }
        }
    }
}

bool
ElevationLayerVector::populateHeightFieldAndNormalMap(osg::HeightField*      hf,
                                                      NormalMap*             normalMap,
                                                      const TileKey&         key,
                                                      const Profile*         haeProfile,
                                                      RasterInterpolation interpolation,
                                                      ProgressCallback*      progress ) const
{
    // heightfield must already exist.
    if ( !hf )
        return false;

    OE_PROFILING_ZONE;

    // if the caller provided an "HAE map profile", he wants an HAE elevation grid even if
    // the map profile has a vertical datum. This is the usual case when building the 3D
    // terrain, for example. Construct a temporary key that doesn't have the vertical
    // datum info and use that to query the elevation data.
    TileKey keyToUse = key;
    if ( haeProfile )
    {
        keyToUse = TileKey(key.getLOD(), key.getTileX(), key.getTileY(), haeProfile );
    }

    // Collect the valid layers for this tile.
    LayerDataVector contenders;
    LayerDataVector offsets;

#ifdef ANALYZE
    struct LayerAnalysis {
        LayerAnalysis() : samples(0), used(false), failed(false), fallback(false), actualKeyValid(true) { }
        int samples; bool used; bool failed; bool fallback; bool actualKeyValid; std::string message;
    };
    std::map<ElevationLayer*, LayerAnalysis> layerAnalysis;
#endif

    int i;

    // Track the number of layers that would return fallback data.
    // if ALL layers would provide fallback data, we can exit early
    // and return nothing.
    unsigned numFallbackLayers = 0;

    // Check them in reverse order since the highest priority is last.
    for (i = size()-1; i>=0; --i)
    {
        ElevationLayer* layer = (*this)[i].get();

        if ( layer->getEnabled() && layer->getVisible() ) // redundant for elevation layers..
        {
            // calculate the resolution-mapped key (adjusted for tile resolution differential).            
            TileKey mappedKey = keyToUse.mapResolution(
                hf->getNumColumns(),
                layer->getTileSize() );

            bool useLayer = true;
            TileKey bestKey( mappedKey );

            // Check whether the non-mapped key is valid according to the user's minLevel setting.
            // We wll ignore the maxDataLevel setting, because we account for that by getting
            // the "best available" key later. We must keep these layers around in case we need
            // to fill in empty spots.
            if (key.getLOD() < layer->getMinLevel())
            {
                useLayer = false;
            }

            // GW - this was wrong because it would exclude layers with a maxDataLevel set
            // below the requested LOD ... when in fact we need around for fallback.
            //if ( !layer->isKeyInLegalRange(key) )
            //{
            //    useLayer = false;
            //}
                
            // Find the "best available" mapped key from the tile source:
            else 
            {
                bestKey = layer->getBestAvailableTileKey(mappedKey);
                if (bestKey.valid())
                {
                    // If the bestKey is not the mappedKey, this layer is providing
                    // fallback data (data at a lower resolution than requested)
                    if ( mappedKey != bestKey )
                    {
                        numFallbackLayers++;
                    }
                }
                else
                {
                    useLayer = false;
                }
            }

            if ( useLayer )
            {
                if ( layer->isOffset() )
                {
                    offsets.push_back(LayerData());
                    LayerData& ld = offsets.back();
                    ld.layer = layer;
                    ld.key = bestKey;
                    ld.index = i;
                }
                else
                {
                    contenders.push_back(LayerData());
                    LayerData& ld = contenders.back();
                    ld.layer = layer;
                    ld.key = bestKey;
                    ld.index = i;
                }

#ifdef ANALYZE
                layerAnalysis[layer].used = true;
#endif
            }
        }
    }

    // nothing? bail out.
    if ( contenders.empty() && offsets.empty() )
    {
        return false;
    }

    // if everything is fallback data, bail out.
    if ( contenders.size() + offsets.size() == numFallbackLayers )
    {
        return false;
    }
    
    // Sample the layers into our target.
    unsigned numColumns = hf->getNumColumns();
    unsigned numRows    = hf->getNumRows();    
    double   xmin       = key.getExtent().xMin();
    double   ymin       = key.getExtent().yMin();
    double   dx         = key.getExtent().width() / (double)(numColumns-1);
    double   dy         = key.getExtent().height() / (double)(numRows-1);
   
    // We will load the actual heightfields on demand. We might not need them all.
    GeoHeightFieldVector heightFields(contenders.size());
    GeoHeightFieldVector offsetFields(offsets.size());
    std::vector<bool>    heightFallback(contenders.size(), false);
    std::vector<bool>    heightFailed(contenders.size(), false);
    std::vector<bool>    offsetFailed(offsets.size(), false);

    // The maximum number of heightfields to keep in this local cache
    const unsigned maxHeightFields = 50;
    unsigned numHeightFieldsInCache = 0;

    const SpatialReference* keySRS = keyToUse.getProfile()->getSRS();

    bool realData = false;

    unsigned int total = numColumns * numRows;

    // query resolution interval (x, y) of each sample IF a normal map is requested.
    osg::ref_ptr<osg::ShortArray> deltaLOD;
    if (normalMap)
    {
        deltaLOD = new osg::ShortArray(total);
    }
    
    int nodataCount = 0;

    TileKey scratchKey; // Storage if a new key needs to be constructed

    bool requiresResample = true;

    // If we only have a single contender layer, and the tile is the same size as the requested 
    // heightfield then we just use it directly and avoid having to resample it
    if (contenders.size() == 1 && offsets.empty())
    {
        ElevationLayer* layer = contenders[0].layer.get();
        TileKey& contenderKey = contenders[0].key;

        GeoHeightField layerHF = layer->createHeightField(contenderKey, 0);
        if (layerHF.valid())
        {
            if (layerHF.getHeightField()->getNumColumns() == hf->getNumColumns() &&
                layerHF.getHeightField()->getNumRows() == hf->getNumRows())
            {
                requiresResample = false;
                memcpy(hf->getFloatArray()->asVector().data(),
                    layerHF.getHeightField()->getFloatArray()->asVector().data(),
                    sizeof(float) * hf->getFloatArray()->size()
                );
                if (deltaLOD.valid())
                {
                    deltaLOD->resize(hf->getFloatArray()->size(), 0);
                }
                realData = true;
            }
        }
    }

    // If we need to mosaic multiple layers or resample it to a new output tilesize go through a resampling loop.
    if (requiresResample)
    {
        for (unsigned c = 0; c < numColumns; ++c)
        {
            double x = xmin + (dx * (double)c);

            // periodically check for cancelation
            if (progress && progress->isCanceled())
            {
                return false;
            }

            for (unsigned r = 0; r < numRows; ++r)
            {
                double y = ymin + (dy * (double)r);

                // Collect elevations from each layer as necessary.
                int resolvedIndex = -1;

                osg::Vec3 normal_sum(0, 0, 0);

                for (int i = 0; i < contenders.size() && resolvedIndex < 0; ++i)
                {
                    ElevationLayer* layer = contenders[i].layer.get();
                    TileKey& contenderKey = contenders[i].key;
                    int index = contenders[i].index;

                    if (heightFailed[i])
                        continue;

                    TileKey* actualKey = &contenderKey;

                    GeoHeightField& layerHF = heightFields[i];

                    if (!layerHF.valid())
                    {
                        // We couldn't get the heightfield from the cache, so try to create it.
                        // We also fallback on parent layers to make sure that we have data at the location even if it's fallback.
                        while (!layerHF.valid() && actualKey->valid() && layer->isKeyInLegalRange(*actualKey))
                        {
                            layerHF = layer->createHeightField(*actualKey, progress);
                            if (!layerHF.valid())
                            {
                                if (actualKey != &scratchKey)
                                {
                                    scratchKey = *actualKey;
                                    actualKey = &scratchKey;
                                }
                                *actualKey = actualKey->createParentKey();
                            }
                        }

                        // Mark this layer as fallback if necessary.
                        if (layerHF.valid())
                        {
                            heightFallback[i] = (*actualKey != contenderKey); // actualKey != contenders[i].second;
                            numHeightFieldsInCache++;
                        }
                        else
                        {
                            heightFailed[i] = true;
#ifdef ANALYZE
                            layerAnalysis[layer].failed = true;
                            layerAnalysis[layer].actualKeyValid = actualKey->valid();
                            if (progress) layerAnalysis[layer].message = progress->message();
#endif
                            continue;
                        }
                    }

                    if (layerHF.valid())
                    {
                        bool isFallback = heightFallback[i];
#ifdef ANALYZE
                        layerAnalysis[layer].fallback = isFallback;
#endif

                        // We only have real data if this is not a fallback heightfield.
                        if (!isFallback)
                        {
                            realData = true;
                        }

                        float elevation;
                        if (layerHF.getElevation(keySRS, x, y, interpolation, keySRS, elevation))
                        {
                            if (elevation != NO_DATA_VALUE)
                            {
                                // remember the index so we can only apply offset layers that
                                // sit on TOP of this layer.
                                resolvedIndex = index;

                                hf->setHeight(c, r, elevation);

#ifdef ANALYZE
                                layerAnalysis[layer].samples++;
#endif

                                if (deltaLOD.valid())
                                {
                                    (*deltaLOD)[r*numColumns + c] = key.getLOD() - actualKey->getLOD();
                                }
                            }
                            else
                            {
                                ++nodataCount;
                            }
                        }
                    }


                    // Clear the heightfield cache if we have too many heightfields in the cache.
                    if (numHeightFieldsInCache >= maxHeightFields)
                    {
                        //OE_NOTICE << "Clearing cache" << std::endl;
                        for (unsigned int k = 0; k < heightFields.size(); k++)
                        {
                            heightFields[k] = GeoHeightField::INVALID;
                            heightFallback[k] = false;
                        }
                        numHeightFieldsInCache = 0;
                    }
                }

                for (int i = offsets.size() - 1; i >= 0; --i)
                {
                    // Only apply an offset layer if it sits on top of the resolved layer
                    // (or if there was no resolved layer).
                    if (resolvedIndex >= 0 && offsets[i].index < resolvedIndex)
                        continue;

                    TileKey &contenderKey = offsets[i].key;

                    if (offsetFailed[i] == true)
                        continue;

                    GeoHeightField& layerHF = offsetFields[i];
                    if (!layerHF.valid())
                    {
                        ElevationLayer* offset = offsets[i].layer.get();

                        layerHF = offset->createHeightField(contenderKey, progress);
                        if (!layerHF.valid())
                        {
                            offsetFailed[i] = true;
                            continue;
                        }
                    }

                    // If we actually got a layer then we have real data
                    realData = true;

                    float elevation = 0.0f;
                    if (layerHF.getElevation(keySRS, x, y, interpolation, keySRS, elevation) &&
                        elevation != NO_DATA_VALUE)
                    {
                        hf->getHeight(c, r) += elevation;

                        // Update the resolution tracker to account for the offset. Sadly this
                        // will wipe out the resolution of the actual data, and might result in 
                        // normal faceting. See the comments on "createNormalMap" for more info
                        if (deltaLOD.valid())
                        {
                            (*deltaLOD)[r*numColumns + c] = key.getLOD() - contenderKey.getLOD();
                        }
                    }
                }
            }
        }
    }

    if (normalMap)
    {
        // periodically check for cancelation
        if (progress && progress->isCanceled())
        {
            return false;
        }

        createNormalMap(key.getExtent(), hf, deltaLOD.get(), normalMap);
    }

#ifdef ANALYZE
    {
        static Threading::Mutex m;
        Threading::ScopedMutexLock lock(m);
        std::cout << key.str() << ": ";
        for (std::map<ElevationLayer*, LayerAnalysis>::const_iterator i = layerAnalysis.begin();
            i != layerAnalysis.end(); ++i)
        {
            std::cout << i->first->getName() 
                << " used=" << i->second.used
                << " failed=" << i->second.failed
                << " akv=" << i->second.actualKeyValid
                << " fallback=" << i->second.fallback
                << " samples=" << i->second.samples
                << " msg=" << i->second.message
                << "; ";
        }
        std::cout << std::endl;
    }
#endif

    // Resolve any invalid heights in the output heightfield.
    HeightFieldUtils::resolveInvalidHeights(hf, key.getExtent(), NO_DATA_VALUE, 0L);

    if (progress && progress->isCanceled())
    {
        return false;
    }

    // Return whether or not we actually read any real data
    return realData;
}
