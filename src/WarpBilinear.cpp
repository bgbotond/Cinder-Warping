/*
 Copyright (c) 2010-2013, Paul Houx - All rights reserved.
 This code is intended for use with the Cinder C++ library: http://libcinder.org

 This file is part of Cinder-Warping.

 Cinder-Warping is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 Cinder-Warping is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with Cinder-Warping.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "WarpBilinear.h"

#include "cinder/Xml.h"
#include "cinder/app/AppBasic.h"
#include "cinder/gl/Context.h"
#include "cinder/gl/Texture.h"

// Cinder does not provide comparison operators on gl::Fbo::Format

bool operator==( const ci::gl::Fbo::Format& a, const ci::gl::Fbo::Format& b )
{
	/*
	if( a.getTarget() != b.getTarget() ) return false;
	if( a.getColorInternalFormat() != b.getColorInternalFormat() ) return false;
	if( a.getDepthInternalFormat() != b.getDepthInternalFormat() ) return false;
	if( a.hasColorBuffer() != b.hasColorBuffer() ) return false;
	if( a.getNumColorBuffers() != b.getNumColorBuffers() ) return false;
	if( a.hasDepthBuffer() != b.hasDepthBuffer() ) return false;
	if( a.hasDepthBufferTexture() != b.hasDepthBufferTexture() ) return false;
	if( a.getSamples() != b.getSamples() ) return false;
	if( a.getCoverageSamples() != b.getCoverageSamples() ) return false;
	if( a.hasMipMapping() != b.hasMipMapping() ) return false;
	*/
	// mWrapS, mWrapT, mMinFilter, mMagFilter are not accessible

	return true;
}

bool operator!=( const ci::gl::Fbo::Format& a, const ci::gl::Fbo::Format& b )
{
	return !( a == b );
}

//

using namespace ci;
using namespace ci::app;

namespace ph {
namespace warping {

WarpBilinear::WarpBilinear( const ci::gl::Fbo::Format &format )
	: Warp( BILINEAR ), mIsLinear( false ), mIsAdaptive( true ), mX1( 0.0f ), mY1( 0.0f ), mX2( 1.0f ), mY2( 1.0f )
	, mResolutionX( 0 ), mResolutionY( 0 ), mFboFormat( format ), mResolution( 16 ) // higher value is coarser mesh
{
	reset();
}

WarpBilinear::~WarpBilinear()
{
}

XmlTree WarpBilinear::toXml() const
{
	XmlTree xml = Warp::toXml();

	// add attributes specific to this type of warp
	xml.setAttribute( "resolution", mResolution );
	xml.setAttribute( "linear", mIsLinear );
	xml.setAttribute( "adaptive", mIsAdaptive );

	return xml;
}

void WarpBilinear::fromXml( const XmlTree &xml )
{
	Warp::fromXml( xml );

	// retrieve attributes specific to this type of warp
	mResolution = xml.getAttributeValue<int>( "resolution", 16 );
	mIsLinear = xml.getAttributeValue<bool>( "linear", false );
	mIsAdaptive = xml.getAttributeValue<bool>( "adaptive", false );
}

void WarpBilinear::setFormat( const gl::Fbo::Format &format )
{
	mFboFormat = format;

	// invalidate current frame buffer
	mFbo.reset();
}

void WarpBilinear::reset()
{
	mPoints.clear();
	for( int x = 0; x < mControlsX; x++ ) {
		for( int y = 0; y < mControlsY; y++ ) {
			mPoints.push_back( vec2( x / float( mControlsX - 1 ), y / float( mControlsY - 1 ) ) );
		}
	}

	mIsDirty = true;
}

void WarpBilinear::draw( const gl::Texture2dRef &texture, const Area &srcArea, const Rectf &destRect )
{
	gl::ScopedTextureBind tex0( texture );

	// clip against bounds
	Area	area = srcArea;
	Rectf	rect = destRect;
	clip( area, rect );

	// set texture coordinates
	float w = static_cast<float>( texture->getWidth() );
	float h = static_cast<float>( texture->getHeight() );

	if( texture->getTarget() == GL_TEXTURE_RECTANGLE_ARB )
		setTexCoords( (float) area.x1, (float) area.y1, (float) area.x2, (float) area.y2 );
	else
		setTexCoords( area.x1 / w, area.y1 / h, area.x2 / w, area.y2 / h );

	// draw
	draw();
}

void WarpBilinear::begin()
{
	// check if the FBO was created and is of the correct size
	if( !mFbo ) {
		try { mFbo = gl::Fbo::create( mWidth, mHeight, mFboFormat ); }
		catch( ... ) {
			// try creating Fbo with default format settings
			try { mFbo = gl::Fbo::create( mWidth, mHeight ); }
			catch( ... ) { return; }
		}
	}
	else if( mFbo->getWidth() != mWidth || mFbo->getHeight() != mHeight || mFbo->getFormat() != mFboFormat ) {
		try { mFbo = gl::Fbo::create( mWidth, mHeight, mFboFormat ); }
		catch( ... ) {
			// try creating Fbo with default format settings
			try { mFbo = gl::Fbo::create( mWidth, mHeight ); }
			catch( ... ) { return; }
		}
	}

	// bind the frame buffer so we can draw to the FBO
	mFbo->bindFramebuffer();

	// store current viewport and set viewport to frame buffer size
	gl::pushViewport( gl::getViewport() );
	gl::viewport( ivec2( 0 ), mFbo->getSize() );

	// set window matrices
	gl::pushMatrices();
	gl::setMatricesWindow( mWidth, mHeight );
}

void WarpBilinear::end()
{
	if( !mFbo ) return;

	// restore matrices
	gl::popMatrices();

	// restore viewport
	gl::popViewport();

	// unbind frame buffer
	mFbo->unbindFramebuffer();

	// draw flipped
	Area srcArea = mFbo->getBounds();
	int32_t t = srcArea.y1; srcArea.y1 = srcArea.y2; srcArea.y2 = t;

	draw( mFbo->getColorTexture(), srcArea, Rectf( getBounds() ) );
}

void WarpBilinear::draw( bool controls )
{
	createBuffers();

	if( !mVboMesh ) return;

	// save current texture mode, drawing color, line width and depth buffer state
	const ColorA &currentColor = gl::context()->getCurrentColor();

	gl::ScopedColor color( currentColor );
	gl::ScopedState disableDepthRead( GL_DEPTH_TEST, GL_FALSE );
	gl::ScopedState disableDepthWrite( GL_DEPTH_WRITEMASK, GL_FALSE );

	glHint( GL_LINE_SMOOTH_HINT, GL_NICEST );

	// adjust brightness
	if( mBrightness < 1.f ) {
		ColorA drawColor = mBrightness * currentColor;
		drawColor.a = currentColor.a;

		gl::color( drawColor );
	}

	// draw textured mesh
	mBatch->draw();

	// draw edit interface
	if( isEditModeEnabled() ) {
		gl::ScopedGlslProg shader( gl::context()->getStockShader( gl::ShaderDef().color() ) );

		/*// draw wireframe
		gl::color( ColorA( 1, 1, 1, 0.5f ) );
		gl::enableAlphaBlending();
		gl::enableWireframe();
		gl::draw( mVboMesh );
		gl::disableAlphaBlending();
		gl::disableWireframe();
		//*/

		if( controls ) {
			// draw control points
			for( unsigned i = 0; i < mPoints.size(); i++ )
				drawControlPoint( getControlPoint( i ) * mWindowSize, i == mSelected );
		}
	}
}

void WarpBilinear::keyDown( KeyEvent &event )
{
	// let base class handle keys first
	Warp::keyDown( event );
	if( event.isHandled() )
		return;

	// disable keyboard input when not in edit mode
	if( !isEditModeEnabled() ) return;

	// do not listen to key input if not selected
	if( mSelected >= mPoints.size() ) return;

	switch( event.getCode() ) {
	case KeyEvent::KEY_F1:
		// reduce the number of horizontal control points
		if( !event.isShiftDown() )
			setNumControlX( ( mControlsX + 1 ) / 2 );
		else setNumControlX( mControlsX - 1 );
		break;
	case KeyEvent::KEY_F2:
		// increase the number of horizontal control points
		if( !event.isShiftDown() )
			setNumControlX( 2 * mControlsX - 1 );
		else setNumControlX( mControlsX + 1 );
		break;
	case KeyEvent::KEY_F3:
		// reduce the number of vertical control points
		if( !event.isShiftDown() )
			setNumControlY( ( mControlsY + 1 ) / 2 );
		else setNumControlY( mControlsY - 1 );
		break;
	case KeyEvent::KEY_F4:
		// increase the number of vertical control points
		if( !event.isShiftDown() )
			setNumControlY( 2 * mControlsY - 1 );
		else setNumControlY( mControlsY + 1 );
		break;
	case KeyEvent::KEY_m:
		// toggle between linear and curved mapping
		mIsLinear = !mIsLinear;
		mIsDirty = true;
		break;
	case KeyEvent::KEY_F5:
		// decrease the mesh resolution
		if( mResolution < 64 ) {
			mResolution += 4;
			mIsDirty = true;
		}
		break;
	case KeyEvent::KEY_F6:
		// increase the mesh resolution
		if( mResolution > 4 ) {
			mResolution -= 4;
			mIsDirty = true;
		}
		break;
	case KeyEvent::KEY_F7:
		// toggle adaptive mesh resolution
		mIsAdaptive = !mIsAdaptive;
		mIsDirty = true;
		break;
	case KeyEvent::KEY_F9:
		// rotate content ccw
		break;
	case KeyEvent::KEY_F10:
		// rotate content cw
		break;
	case KeyEvent::KEY_F11:
	{
		// flip control points horizontally
		std::vector<vec2> points;
		for( int x = mControlsX - 1; x >= 0; --x ) {
			for( int y = 0; y < mControlsY; ++y ) {
				int i = ( x * mControlsY + y );
				points.push_back( mPoints[i] );
			}
		}
		mPoints = points;
		mIsDirty = true;
	}
		break;
	case KeyEvent::KEY_F12:
	{
		// flip control points vertically
		std::vector<vec2> points;
		for( int x = 0; x < mControlsX; ++x ) {
			for( int y = mControlsY - 1; y >= 0; --y ) {
				int i = ( x * mControlsY + y );
				points.push_back( mPoints[i] );
			}
		}
		mPoints = points;
		mIsDirty = true;
	}
		break;
	default:
		return;
	}

	event.setHandled( true );
}

void WarpBilinear::createBuffers()
{
	if( mIsDirty ) {
		if( mIsAdaptive ) {
			// determine a suitable mesh resolution based on width/height of the window
			// and the size of the mesh in pixels
			Rectf rect = getMeshBounds();
			createMesh( (int) ( rect.getWidth() / mResolution ), (int) ( rect.getHeight() / mResolution ) );
		}
		else {
			// use a fixed mesh resolution
			createMesh( mWidth / mResolution, mHeight / mResolution );
		}
		updateMesh();
	}
}

void WarpBilinear::createMesh( int resolutionX, int resolutionY )
{
	// convert from number of quads to number of vertices
	++resolutionX;	++resolutionY;

	// find a value for resolutionX and resolutionY that can be
	// evenly divided by mControlsX and mControlsY
	if( mControlsX < resolutionX ) {
		int dx = ( resolutionX - 1 ) % ( mControlsX - 1 );
		if( dx >= ( mControlsX / 2 ) ) dx -= ( mControlsX - 1 );
		resolutionX -= dx;
	}
	else {
		resolutionX = mControlsX;
	}

	if( mControlsY < resolutionY ) {
		int dy = ( resolutionY - 1 ) % ( mControlsY - 1 );
		if( dy >= ( mControlsY / 2 ) ) dy -= ( mControlsY - 1 );
		resolutionY -= dy;
	}
	else {
		resolutionY = mControlsY;
	}

	//
	mResolutionX = resolutionX;
	mResolutionY = resolutionY;

	//
	int numVertices = ( resolutionX * resolutionY );
	int numTris = 2 * ( resolutionX - 1 ) * ( resolutionY - 1 );
	int numIndices = numTris * 3;

	//
	gl::VboMesh::Layout	layout;
	layout.interleave( false );
	layout.attrib( geom::POSITION, 3 );
	layout.attrib( geom::TEX_COORD_0, 2 );
	layout.usage( GL_DYNAMIC_DRAW );

	//
	mVboMesh = gl::VboMesh::create( numVertices, GL_TRIANGLES, { layout }, numIndices, GL_UNSIGNED_INT );
	if( !mVboMesh ) return;

	// buffer static data
	int i = 0;
	int j = 0;
	std::vector<uint32_t>	indices( numIndices );
	std::vector<vec2>		texCoords( numVertices );
	for( int x = 0; x < resolutionX; ++x ) {
		for( int y = 0; y < resolutionY; ++y ) {
			// index
			if( ( ( x + 1 ) < resolutionX ) && ( ( y + 1 ) < resolutionY ) ) {
				indices[i++] = ( x + 0 ) * resolutionY + ( y + 0 );
				indices[i++] = ( x + 1 ) * resolutionY + ( y + 0 );
				indices[i++] = ( x + 1 ) * resolutionY + ( y + 1 );

				indices[i++] = ( x + 0 ) * resolutionY + ( y + 0 );
				indices[i++] = ( x + 1 ) * resolutionY + ( y + 1 );
				indices[i++] = ( x + 0 ) * resolutionY + ( y + 1 );
			}
			// texCoords
			float tx = lerp<float, float>( mX1, mX2, x / (float) ( resolutionX - 1 ) );
			float ty = lerp<float, float>( mY1, mY2, y / (float) ( resolutionY - 1 ) );
			texCoords[j++] = vec2( tx, ty );
		}
	}
	mVboMesh->bufferIndices( indices.size() * sizeof( uint32_t ), indices.data() );
	mVboMesh->bufferAttrib( geom::TEX_COORD_0, texCoords.size() * sizeof( vec2 ), texCoords.data() );

	//
	std::vector<vec3>	positions;
	positions.resize( mResolutionX * mResolutionY );
	mVboMesh->bufferAttrib( geom::POSITION, positions.size() * sizeof( vec3 ), positions.data() );

	//
	mIsDirty = true;
}

void WarpBilinear::updateMesh()
{
	if( !mVboMesh ) return;
	if( !mIsDirty ) return;

	vec2			p;
	float			u, v;
	int				col, row;

	std::vector<vec2>	cols, rows;
	
	auto mapped = mVboMesh->mapAttrib3f( geom::POSITION, false );
	int index = 0;
	for( int x = 0; x < mResolutionX; ++x ) {
		for( int y = 0; y < mResolutionY; ++y ) {
			// transform coordinates to [0..numControls]
			u = x * ( mControlsX - 1 ) / (float) ( mResolutionX - 1 );
			v = y * ( mControlsY - 1 ) / (float) ( mResolutionY - 1 );

			// determine col and row
			col = (int) ( u );
			row = (int) ( v );

			// normalize coordinates to [0..1]
			u -= col;
			v -= row;

			if( mIsLinear ) {
				// perform linear interpolation
				vec2 p1 = ( 1.0f - u ) * getPoint( col, row ) + u * getPoint( col + 1, row );
				vec2 p2 = ( 1.0f - u ) * getPoint( col, row + 1 ) + u * getPoint( col + 1, row + 1 );
				p = ( ( 1.0f - v ) * p1 + v * p2 ) * mWindowSize;
			}
			else {
				// perform bicubic interpolation
				rows.clear();
				for( int i = -1; i < 3; ++i ) {
					cols.clear();
					for( int j = -1; j < 3; ++j ) {
						cols.push_back( getPoint( col + i, row + j ) );
					}
					rows.push_back( cubicInterpolate( cols, v ) );
				}
				p = cubicInterpolate( rows, u ) * mWindowSize;
			}

			*mapped++ = vec3( p.x, p.y, 0 );
		}
	}
	mapped.unmap();

	mBatch = gl::Batch::create( mVboMesh, gl::context()->getStockShader( gl::ShaderDef().texture().color() ) );

	mIsDirty = false;
}

vec2 WarpBilinear::getPoint( int col, int row ) const
{
	int maxCol = mControlsX - 1;
	int maxRow = mControlsY - 1;

	// here's the magic: extrapolate points beyond the edges
	if( col < 0 ) return 2.0f * getPoint( 0, row ) - getPoint( 0 - col, row );
	if( row < 0 ) return 2.0f * getPoint( col, 0 ) - getPoint( col, 0 - row );
	if( col > maxCol ) return 2.0f * getPoint( maxCol, row ) - getPoint( 2 * maxCol - col, row );
	if( row > maxRow ) return 2.0f * getPoint( col, maxRow ) - getPoint( col, 2 * maxRow - row );

	// points on the edges or within the mesh can simply be looked up
	return mPoints[( col * mControlsY ) + row];
}

// from http://www.paulinternet.nl/?page=bicubic : fast catmull-rom calculation
vec2 WarpBilinear::cubicInterpolate( const std::vector<vec2> &knots, float t ) const
{
	assert( knots.size() >= 4 );

	return knots[1] + 0.5f * t*( knots[2] - knots[0] + t*( 2.0f*knots[0] - 5.0f*knots[1] +
		4.0f*knots[2] - knots[3] + t*( 3.0f*( knots[1] - knots[2] ) + knots[3] - knots[0] ) ) );
}

void WarpBilinear::setNumControlX( int n )
{
	// there should be a minimum of 2 control points
	n = math<int>::max( 2, n );

	// create a list of new points
	std::vector<vec2> temp( n * mControlsY );

	// perform spline fitting
	for( int row = 0; row < mControlsY; ++row ) {
		std::vector<vec2> points;
		if( mIsLinear ) {
			// construct piece-wise linear spline
			for( int col = 0; col < mControlsX; ++col ) {
				points.push_back( getPoint( col, row ) );
			}

			BSpline2f s( points, 1, false, true );

			// calculate position of new control points
			float length = s.getLength( 0.0f, 1.0f );
			float step = 1.0f / ( n - 1 );
			for( int col = 0; col < n; ++col ) {
				temp[( col * mControlsY ) + row] = s.getPosition( s.getTime( length * col * step ) );
			}
		}
		else {
			// construct piece-wise catmull-rom spline
			for( int col = 0; col < mControlsX; ++col ) {
				vec2 p0 = getPoint( col - 1, row );
				vec2 p1 = getPoint( col, row );
				vec2 p2 = getPoint( col + 1, row );
				vec2 p3 = getPoint( col + 2, row );

				// control points according to an optimized Catmull-Rom implementation
				vec2 b1 = p1 + ( p2 - p0 ) / 6.0f;
				vec2 b2 = p2 - ( p3 - p1 ) / 6.0f;

				points.push_back( p1 );

				if( col < ( mControlsX - 1 ) ) {
					points.push_back( b1 );
					points.push_back( b2 );
				}
			}

			BSpline2f s( points, 3, false, true );

			// calculate position of new control points
			float length = s.getLength( 0.0f, 1.0f );
			float step = 1.0f / ( n - 1 );
			for( int col = 0; col < n; ++col ) {
				temp[( col * mControlsY ) + row] = s.getPosition( s.getTime( length * col * step ) );
			}
		}
	}

	// copy new control points 
	mPoints = temp;
	mControlsX = n;

	mIsDirty = true;
}

void WarpBilinear::setNumControlY( int n )
{
	// there should be a minimum of 2 control points
	n = math<int>::max( 2, n );

	// create a list of new points
	std::vector<vec2> temp( mControlsX * n );

	// perform spline fitting
	for( int col = 0; col < mControlsX; ++col ) {
		std::vector<vec2> points;
		if( mIsLinear ) {
			// construct piece-wise linear spline
			for( int row = 0; row < mControlsY; ++row )
				points.push_back( getPoint( col, row ) );

			BSpline2f s( points, 1, false, true );

			// calculate position of new control points
			float length = s.getLength( 0.0f, 1.0f );
			float step = 1.0f / ( n - 1 );
			for( int row = 0; row < n; ++row ) {
				temp[( col * n ) + row] = s.getPosition( s.getTime( length * row * step ) );
			}
		}
		else {
			// construct piece-wise catmull-rom spline
			for( int row = 0; row < mControlsY; ++row ) {
				vec2 p0 = getPoint( col, row - 1 );
				vec2 p1 = getPoint( col, row );
				vec2 p2 = getPoint( col, row + 1 );
				vec2 p3 = getPoint( col, row + 2 );

				// control points according to an optimized Catmull-Rom implementation
				vec2 b1 = p1 + ( p2 - p0 ) / 6.0f;
				vec2 b2 = p2 - ( p3 - p1 ) / 6.0f;

				points.push_back( p1 );

				if( row < ( mControlsY - 1 ) ) {
					points.push_back( b1 );
					points.push_back( b2 );
				}
			}

			BSpline2f s( points, 3, false, true );

			// calculate position of new control points
			float length = s.getLength( 0.0f, 1.0f );
			float step = 1.0f / ( n - 1 );
			for( int row = 0; row < n; ++row ) {
				temp[( col * n ) + row] = s.getPosition( s.getTime( length * row * step ) );
			}
		}
	}

	// copy new control points 
	mPoints = temp;
	mControlsY = n;

	mIsDirty = true;
}

Rectf WarpBilinear::getMeshBounds() const
{
	vec2 min = vec2( 1 );
	vec2 max = vec2( 0 );

	for( unsigned i = 0; i < mPoints.size(); ++i ) {
		min.x = math<float>::min( mPoints[i].x, min.x );
		min.y = math<float>::min( mPoints[i].y, min.y );
		max.x = math<float>::max( mPoints[i].x, max.x );
		max.y = math<float>::max( mPoints[i].y, min.y );
	}

	return Rectf( min * mWindowSize, max * mWindowSize );
}

void WarpBilinear::setTexCoords( float x1, float y1, float x2, float y2 )
{
	mIsDirty |= ( x1 != mX1 || y1 != mY1 || x2 != mX2 || y2 != mY2 );
	if( !mIsDirty ) return;

	mX1 = x1;
	mY1 = y1;
	mX2 = x2;
	mY2 = y2;
}

}
} // namespace ph::warping


