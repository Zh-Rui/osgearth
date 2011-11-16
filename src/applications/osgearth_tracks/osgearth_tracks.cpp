/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2010 Pelican Mapping
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
#include <osgEarth/MapNode>
#include <osgEarth/Random>
#include <osgEarth/StringUtils>
#include <osgEarth/ImageUtils>
#include <osgEarth/GeoMath>
#include <osgEarth/Units>
#include <osgEarth/StringUtils>
#include <osgEarthUtil/EarthManipulator>
#include <osgEarthUtil/Formatters>
#include <osgEarthUtil/Controls>
#include <osgEarthAnnotation/TrackNode>
#include <osgEarthAnnotation/Decluttering>
#include <osgEarthAnnotation/AnnotationData>
#include <osgEarthSymbology/Color>

#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <osgGA/StateSetManipulator>
#include <osg/CoordinateSystemNode>
#include <osg/Program>
#include <osg/BlendFunc>

using namespace osgEarth;
using namespace osgEarth::Util;
using namespace osgEarth::Util::Controls;
using namespace osgEarth::Annotation;
using namespace osgEarth::Symbology;

#define LC "[osgearth_tracks] "

/**
 * Demonstrates use of the TrackNode to display entity track symbols.
 */

// field names for the track labels
#define FIELD_NAME     "name"
#define FIELD_POSITION "position"
#define FIELD_NUMBER   "number"

// icon to use, and size in pixels
#define ICON_URL       "../data/m2525_air.png"
#define ICON_SIZE      40

// length of the simulation, in seconds
#define NUM_TRACKS     500
#define SIM_DURATION   60

// format coordinates as MGRS
static MGRSFormatter s_format(MGRSFormatter::PRECISION_1000M);

// globals for this demo
osg::StateSet* g_declutterStateSet = 0L;




/** Prints an error message */
int
usage( const std::string& message )
{
    OE_WARN << LC << message << std::endl;
    return -1;
}


/** A little track simulator that goes a simple great circle interpolation */
struct TrackSim : public osg::Referenced
{
    TrackNode* _track;
    Angular _startLat, _startLon, _endLat, _endLon;

    void update( double t )
    {
        osg::Vec3d pos;
        GeoMath::interpolate(
            _startLat.as(Units::RADIANS), _startLon.as(Units::RADIANS),
            _endLat.as(Units::RADIANS), _endLon.as(Units::RADIANS),
            t,
            pos.y(), pos.x() );

        pos.x() = osg::RadiansToDegrees(pos.x());
        pos.y() = osg::RadiansToDegrees(pos.y());

        // update the position label.
        _track->setPosition(pos);
        _track->setFieldValue( FIELD_POSITION, Stringify() << s_format(pos.y(),pos.x()) );
    }
};
typedef std::list< osg::ref_ptr<TrackSim> > TrackSims;



/**
 * Creates a field schema that we'll later use as a labeling template for
 * TrackNode instances.
 */
void
createFieldSchema( TrackNodeFieldSchema& schema )
{
    // draw the track name above the icon:
    TextSymbol* nameSymbol = new TextSymbol();
    nameSymbol->pixelOffset()->set( 0, 2+ICON_SIZE/2 );
    nameSymbol->alignment() = TextSymbol::ALIGN_CENTER_BOTTOM;
    nameSymbol->halo()->color() = Color::Black;
    nameSymbol->size() = nameSymbol->size().value() + 2.0f;
    schema[FIELD_NAME] = nameSymbol;

    // draw the track coordinates below the icon:
    TextSymbol* posSymbol = new TextSymbol();
    posSymbol->pixelOffset()->set( 0, -2-ICON_SIZE/2 );
    posSymbol->alignment() = TextSymbol::ALIGN_CENTER_TOP;
    posSymbol->halo()->color() = Color::Black;
    schema[FIELD_POSITION] = posSymbol;

    // draw some other field to the left:
    TextSymbol* numberSymbol = new TextSymbol();
    numberSymbol->pixelOffset()->set( -2-ICON_SIZE/2, 0 );
    numberSymbol->alignment() = TextSymbol::ALIGN_RIGHT_CENTER;
    numberSymbol->halo()->color() = Color::Black;
    schema[FIELD_NUMBER] = numberSymbol;
}

/** Builds a bunch of tracks. */
void
createTrackNodes( MapNode* mapNode, osg::Group* parent, const TrackNodeFieldSchema& schema, TrackSims& sims )
{
    // load an icon to use:
    osg::ref_ptr<osg::Image> srcImage = osgDB::readImageFile( ICON_URL );
    osg::ref_ptr<osg::Image> image;
    ImageUtils::resizeImage( srcImage.get(), ICON_SIZE, ICON_SIZE, image );

    // make some tracks, choosing a random simulation for each.
    Random prng;

    for( unsigned i=0; i<NUM_TRACKS; ++i )
    {
        double lon0 = -180.0 + prng.next() * 360.0;
        double lat0 = -80.0 + prng.next() * 160.0;

        TrackNode* track = new TrackNode( mapNode, osg::Vec3d(lon0, lat0, 0), image, schema );

        track->setFieldValue( FIELD_NAME,     Stringify() << "Track:" << i );
        track->setFieldValue( FIELD_POSITION, Stringify() << s_format(lat0, lon0) );
        track->setFieldValue( FIELD_NUMBER,   Stringify() << (1 + prng.next(9)) );

        // add a priority
        AnnotationData* data = new AnnotationData();
        data->setPriority( (float)i );
        track->setUserData( data );

        parent->addChild( track );

        // add a simulator for this guy
        double lon1 = -180.0 + prng.next() * 360.0;
        double lat1 = -80.0 + prng.next() * 160.0;
        TrackSim* sim = new TrackSim();
        sim->_track = track;
        sim->_startLat = lat0; sim->_startLon = lon0;
        sim->_endLat = lat1; sim->_endLon = lon1;
        sims.push_back( sim );
    }
}

/** creates some UI controls for adjusting the decluttering parameters. */
void
createControls( osgViewer::View* view )
{
    ControlCanvas* canvas = ControlCanvas::get(view, true);
    
    // title bar
    VBox* vbox = canvas->addControl(new VBox(Control::ALIGN_NONE, Control::ALIGN_BOTTOM, 2, 1 ));
    vbox->setBackColor( Color(Color::Black, 0.5) );
    vbox->addControl( new LabelControl("osgEarth Tracks Demo", Color::Yellow) );
    
    // checkbox that toggles decluttering of tracks
    struct ToggleDecluttering : public ControlEventHandler {
        void onValueChanged( Control* c, bool on ) {
            g_declutterStateSet->setRenderBinMode( 
                on ? osg::StateSet::USE_RENDERBIN_DETAILS : osg::StateSet::INHERIT_RENDERBIN_DETAILS );
        }
    };
    HBox* toggleBox = vbox->addControl( new HBox() );
    toggleBox->addControl( new CheckBoxControl(true, new ToggleDecluttering()) );
    toggleBox->addControl( new LabelControl("Declutter") );

    // grid for the slider controls so they look nice
    Grid* grid = vbox->addControl( new Grid() );
    grid->setHorizFill( true );
    grid->setChildHorizAlign( Control::ALIGN_LEFT );
    grid->setChildSpacing( 6 );

    unsigned r=0;
    static DeclutteringOptions g_dcOptions = Decluttering::getOptions();

    // event handler for changing decluttering options
    struct ChangeFloatOption : public ControlEventHandler {
        optional<float>& _param;
        LabelControl* _label;
        ChangeFloatOption( optional<float>& param, LabelControl* label ) : _param(param), _label(label) { }
        void onValueChanged( Control* c, float value ) {
            _param = value;
            _label->setText( Stringify() << std::setprecision(1) << value );
            Decluttering::setOptions( g_dcOptions );
        }
    };

    grid->setControl( 0, r, new LabelControl("Scale threshold:") );
    LabelControl* scaleLabel = grid->setControl( 2, r, new LabelControl(Stringify() << std::setprecision(1) << *g_dcOptions.minScale()) );
    HSliderControl* scaleSlider = grid->setControl( 1, r, new HSliderControl( 
        0.0, 1.0, *g_dcOptions.minScale(), new ChangeFloatOption(g_dcOptions.minScale(), scaleLabel) ) );
    scaleSlider->setHorizFill( true, 200 );

    grid->setControl( 0, ++r, new LabelControl("Alpha threshold:") );
    LabelControl* alphaLabel = grid->setControl( 2, r, new LabelControl(Stringify() << std::setprecision(1) << *g_dcOptions.minAlpha()) );
    HSliderControl* alphaSlider = grid->setControl( 1, r, new HSliderControl( 
        0.0, 1.0, *g_dcOptions.minAlpha(), new ChangeFloatOption(g_dcOptions.minAlpha(), alphaLabel) ) );

    grid->setControl( 0, ++r, new LabelControl("Activate speed:") );
    LabelControl* actLabel = grid->setControl( 2, r, new LabelControl(Stringify() << std::setprecision(1) << *g_dcOptions.stepUp()) );
    HSliderControl* actSlider = grid->setControl( 1, r, new HSliderControl( 
        0.01, 0.5, *g_dcOptions.stepUp(), new ChangeFloatOption(g_dcOptions.stepUp(), actLabel) ) );

    grid->setControl( 0, ++r, new LabelControl("Deactivate speed:") );
    LabelControl* deactLabel = grid->setControl( 2, r, new LabelControl(Stringify() << std::setprecision(1) << *g_dcOptions.stepDown()) );
    HSliderControl* deactSlider = grid->setControl( 1, r, new HSliderControl( 
        0.01, 0.5, *g_dcOptions.stepDown(), new ChangeFloatOption(g_dcOptions.stepDown(), deactLabel) ) );
}

/**
 * Main application.
 * Creates some simulated track data and runs the simulation.
 */
int
main(int argc, char** argv)
{
    osg::ArgumentParser arguments(&argc,argv);

    // load a map from an earth file.
    MapNode* mapNode = MapNode::load( arguments );
    if ( !mapNode )
        return usage( "Missing required .earth file" );
    
    osg::Group* root = new osg::Group();
    root->addChild( mapNode );

    // build a track field schema.
    TrackNodeFieldSchema schema;
    createFieldSchema( schema );

    // a list of simulators for our tracks.
    TrackSims trackSims;

    // create some track nodes, and activate decluttering by default.
    osg::Group* tracks = new osg::Group();
    createTrackNodes( mapNode, tracks, schema, trackSims );
    g_declutterStateSet = tracks->getOrCreateStateSet();
    g_declutterStateSet->setRenderBinDetails( INT_MAX, OSGEARTH_DECLUTTER_BIN );
    root->addChild( tracks );

    // initialize a viewer.
    osgViewer::Viewer viewer( arguments );
    viewer.setCameraManipulator( new EarthManipulator );
    viewer.setSceneData( root );

    // configure a UI for controlling the demo
    createControls( &viewer );

    // osgEarth benefits from pre-compilation of GL objects in the pager. In newer versions of
    // OSG, this activates OSG's IncrementalCompileOpeartion in order to avoid frame breaks.
    viewer.getDatabasePager()->setDoPreCompile( true );

    // add some stock OSG handlers:
    viewer.addEventHandler(new osgViewer::StatsHandler());
    viewer.addEventHandler(new osgViewer::WindowSizeHandler());
    viewer.addEventHandler(new osgViewer::ThreadingHandler());
    viewer.addEventHandler(new osgViewer::LODScaleHandler());
    viewer.addEventHandler(new osgGA::StateSetManipulator(viewer.getCamera()->getOrCreateStateSet()));
    viewer.addEventHandler(new osgViewer::HelpHandler(arguments.getApplicationUsage()));

    while( !viewer.done() )
    {
        viewer.frame();

        double t = fmod(viewer.getFrameStamp()->getSimulationTime(), SIM_DURATION) / SIM_DURATION;

        for( TrackSims::iterator i = trackSims.begin(); i != trackSims.end(); ++i )
        {
            i->get()->update( t );
        }
    }
}
