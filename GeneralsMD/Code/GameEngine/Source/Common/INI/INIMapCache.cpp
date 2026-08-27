/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: INIMapCache.cpp ///////////////////////////////////////////////////////////////////////////
// Author: Matthew D. Campbell, February 2002
// Desc:   Parsing MapCache INI entries
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Lib/BaseType.h"
#include "Common/INI.h"
#include "GameClient/MapUtil.h"
#include "GameClient/GameText.h"
#include "GameNetwork/NetworkDefs.h"
#include "Common/NameKeyGenerator.h"
#include "Common/WellKnownKeys.h"
#include "Common/QuotedPrintable.h"


class MapMetaDataReader
{
public:
	MapMetaDataReader()
	{
		// Path-distance fields are optional in the cache INI (only pairs the
		// build actually computed get written), so they must default to 0
		// ("not computed") rather than stack garbage.
		Int k;
		for (k = 0; k < MAX_SLOTS * (MAX_SLOTS - 1) / 2; ++k)
			m_startSpotPathDist[k] = 0.0f;
	}

	Region3D m_extent;
	Int m_numPlayers;
	Bool m_isMultiplayer;
	AsciiString m_asciiDisplayName;
	AsciiString m_asciiNameLookupTag;

	Bool m_isOfficial;
	WinTimeStamp m_timestamp;
	UnsignedInt m_filesize;
	UnsignedInt m_CRC;

	Coord3D m_waypoints[MAX_SLOTS];
	Real m_startSpotPathDist[MAX_SLOTS * (MAX_SLOTS - 1) / 2];
	Coord3D m_initialCameraPosition;
	Coord3DList m_supplyPositions;
	Coord3DList m_techPositions;
	Coord3DList m_cratePositions;
	Coord3DList m_techDerrickPositions;
	Coord3DList m_garrisonablePositions;
	MapBridgeSpanList m_bridgeSpans;
	static const FieldParse m_mapFieldParseTable[];		///< the parse table for INI definition
	const FieldParse *getFieldParse() const { return m_mapFieldParseTable; }
};


void parseSupplyPositionCoord3D( INI* ini, void * instance, void * /*store*/, const void* /*userData*/ )
{
	MapMetaDataReader *mmdr = (MapMetaDataReader *)instance;
	Coord3D coord3d;
	INI::parseCoord3D(ini, nullptr, &coord3d,nullptr );
	mmdr->m_supplyPositions.push_front(coord3d);

}

void parseTechPositionsCoord3D( INI* ini, void * instance, void * /*store*/, const void* /*userData*/ )
{
	MapMetaDataReader *mmdr = (MapMetaDataReader *)instance;
	Coord3D coord3d;
	INI::parseCoord3D(ini, nullptr, &coord3d,nullptr );
	mmdr->m_techPositions.push_front(coord3d);

}

void parseCratePositionCoord3D( INI* ini, void * instance, void * /*store*/, const void* /*userData*/ )
{
	MapMetaDataReader *mmdr = (MapMetaDataReader *)instance;
	Coord3D coord3d;
	INI::parseCoord3D(ini, nullptr, &coord3d, nullptr);
	mmdr->m_cratePositions.push_front(coord3d);
}

void parseTechDerrickPositionCoord3D( INI* ini, void * instance, void * /*store*/, const void* /*userData*/ )
{
	MapMetaDataReader *mmdr = (MapMetaDataReader *)instance;
	Coord3D coord3d;
	INI::parseCoord3D(ini, nullptr, &coord3d, nullptr);
	mmdr->m_techDerrickPositions.push_front(coord3d);
}

void parseGarrisonablePositionCoord3D( INI* ini, void * instance, void * /*store*/, const void* /*userData*/ )
{
	MapMetaDataReader *mmdr = (MapMetaDataReader *)instance;
	Coord3D coord3d;
	INI::parseCoord3D(ini, nullptr, &coord3d, nullptr);
	mmdr->m_garrisonablePositions.push_front(coord3d);
}

void parseBridgeSpanEntry( INI* ini, void * instance, void * /*store*/, const void* /*userData*/ )
{
	MapMetaDataReader *mmdr = (MapMetaDataReader *)instance;
	MapBridgeSpan span;
	span.m_templateName = QuotedPrintableToAsciiString(ini->getNextAsciiString());
	INI::parseCoord3D(ini, nullptr, &span.m_from, nullptr);
	INI::parseCoord3D(ini, nullptr, &span.m_to, nullptr);
	mmdr->m_bridgeSpans.push_back(span);
}

const FieldParse MapMetaDataReader::m_mapFieldParseTable[] =
{

	{ "isOfficial",							INI::parseBool,			nullptr,	offsetof( MapMetaDataReader, m_isOfficial ) },
	{ "isMultiplayer",					INI::parseBool,			nullptr,	offsetof( MapMetaDataReader, m_isMultiplayer ) },
	{ "extentMin",							INI::parseCoord3D,	nullptr, offsetof( MapMetaDataReader, m_extent.lo ) },
	{ "extentMax",							INI::parseCoord3D,	nullptr, offsetof( MapMetaDataReader, m_extent.hi ) },
	{ "numPlayers",							INI::parseInt,			nullptr,	offsetof( MapMetaDataReader, m_numPlayers ) },
	{ "fileSize",								INI::parseUnsignedInt,	nullptr,	offsetof( MapMetaDataReader, m_filesize ) },
	{ "fileCRC",								INI::parseUnsignedInt,	nullptr,	offsetof( MapMetaDataReader, m_CRC ) },
	{ "timestampLo",						INI::parseInt,			nullptr,	offsetof( MapMetaDataReader, m_timestamp.m_lowTimeStamp ) },
	{ "timestampHi",						INI::parseInt,			nullptr,	offsetof( MapMetaDataReader, m_timestamp.m_highTimeStamp ) },
	{ "displayName",						INI::parseAsciiString,	nullptr,	offsetof( MapMetaDataReader, m_asciiDisplayName ) },
	{ "nameLookupTag",					INI::parseAsciiString,	nullptr,	offsetof( MapMetaDataReader, m_asciiNameLookupTag ) },

	{ "supplyPosition",					parseSupplyPositionCoord3D,	nullptr, 0 },
	{ "techPosition",						parseTechPositionsCoord3D,	nullptr, 0 },
	{ "cratePosition",					parseCratePositionCoord3D,	nullptr, 0 },
	{ "techDerrickPosition",		parseTechDerrickPositionCoord3D,	nullptr, 0 },
	{ "garrisonablePosition",		parseGarrisonablePositionCoord3D,	nullptr, 0 },
	{ "bridgeSpan",						parseBridgeSpanEntry,	nullptr, 0 },

	{ "Player_1_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) },
	{ "Player_2_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 1 },
	{ "Player_3_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 2 },
	{ "Player_4_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 3 },
	{ "Player_5_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 4 },
	{ "Player_6_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 5 },
	{ "Player_7_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 6 },
	{ "Player_8_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 7 },

	// Ground-path distances between start spots (1-based spot pairs), flat
	// array in (i ascending, j ascending) order. See MapCache::writeCacheINI.
	{ "startSpotPathDist_1_2",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 0 },
	{ "startSpotPathDist_1_3",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 1 },
	{ "startSpotPathDist_1_4",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 2 },
	{ "startSpotPathDist_1_5",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 3 },
	{ "startSpotPathDist_1_6",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 4 },
	{ "startSpotPathDist_1_7",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 5 },
	{ "startSpotPathDist_1_8",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 6 },
	{ "startSpotPathDist_2_3",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 7 },
	{ "startSpotPathDist_2_4",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 8 },
	{ "startSpotPathDist_2_5",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 9 },
	{ "startSpotPathDist_2_6",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 10 },
	{ "startSpotPathDist_2_7",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 11 },
	{ "startSpotPathDist_2_8",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 12 },
	{ "startSpotPathDist_3_4",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 13 },
	{ "startSpotPathDist_3_5",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 14 },
	{ "startSpotPathDist_3_6",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 15 },
	{ "startSpotPathDist_3_7",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 16 },
	{ "startSpotPathDist_3_8",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 17 },
	{ "startSpotPathDist_4_5",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 18 },
	{ "startSpotPathDist_4_6",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 19 },
	{ "startSpotPathDist_4_7",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 20 },
	{ "startSpotPathDist_4_8",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 21 },
	{ "startSpotPathDist_5_6",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 22 },
	{ "startSpotPathDist_5_7",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 23 },
	{ "startSpotPathDist_5_8",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 24 },
	{ "startSpotPathDist_6_7",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 25 },
	{ "startSpotPathDist_6_8",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 26 },
	{ "startSpotPathDist_7_8",	INI::parseReal,	nullptr,	offsetof( MapMetaDataReader, m_startSpotPathDist ) + sizeof(Real) * 27 },

	{ "InitialCameraPosition",	INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_initialCameraPosition ) },

	{ nullptr,					nullptr,						nullptr,						0 }

};

void INI::parseMapCacheDefinition( INI* ini )
{
	const char *c;
	AsciiString name;
	MapMetaDataReader mdr;
	MapMetaData md;

	// read the name
	c = ini->getNextToken(" \n\r\t");
	name.set( c );
	name = QuotedPrintableToAsciiString(name);
	md.m_waypoints.clear();

	ini->initFromINI( &mdr, mdr.getFieldParse() );

	md.m_extent = mdr.m_extent;
	md.m_isOfficial = mdr.m_isOfficial != 0;
	md.m_doesExist = TRUE;
	md.m_isMultiplayer = mdr.m_isMultiplayer != 0;
	md.m_numPlayers = mdr.m_numPlayers;
	md.m_filesize = mdr.m_filesize;
	md.m_CRC = mdr.m_CRC;
	md.m_timestamp = mdr.m_timestamp;

	md.m_waypoints[TheNameKeyGenerator->keyToName(TheKey_InitialCameraPosition)] = mdr.m_initialCameraPosition;

#if RTS_GENERALS
	md.m_displayName = QuotedPrintableToUnicodeString(mdr.m_asciiDisplayName);
#else
	md.m_nameLookupTag = QuotedPrintableToAsciiString(mdr.m_asciiNameLookupTag);

	if (md.m_nameLookupTag.isEmpty())
	{
		// maps without localized name tags
		AsciiString tempdisplayname;
		tempdisplayname = name.reverseFind('\\') + 1;
		md.m_displayName.translate(tempdisplayname);
		if (md.m_numPlayers >= 2)
		{
			UnicodeString extension;
			extension.format(L" (%d)", md.m_numPlayers);
			md.m_displayName.concat(extension);
		}
	}
	else
	{
		// official maps with name tags
		md.m_displayName = TheGameText->fetch(md.m_nameLookupTag);
		if (md.m_numPlayers >= 2)
		{
			UnicodeString extension;
			extension.format(L" (%d)", md.m_numPlayers);
			md.m_displayName.concat(extension);
		}
	}
#endif

	AsciiString startingCamName;
	for (Int i=0; i<md.m_numPlayers; ++i)
	{
		startingCamName.format("Player_%d_Start", i+1); // start pos waypoints are 1-based
		md.m_waypoints[startingCamName] = mdr.m_waypoints[i];
	}

	Coord3DList::iterator it = mdr.m_supplyPositions.begin();
	while( it != mdr.m_supplyPositions.end())
	{
		md.m_supplyPositions.push_front(*it);
		it++;
	}

	it = mdr.m_techPositions.begin();
	while( it != mdr.m_techPositions.end())
	{
		md.m_techPositions.push_front(*it);
		it++;
	}

	it = mdr.m_cratePositions.begin();
	while (it != mdr.m_cratePositions.end())
	{
		md.m_cratePositions.push_front(*it);
		it++;
	}

	it = mdr.m_techDerrickPositions.begin();
	while (it != mdr.m_techDerrickPositions.end())
	{
		md.m_techDerrickPositions.push_front(*it);
		it++;
	}

	it = mdr.m_garrisonablePositions.begin();
	while (it != mdr.m_garrisonablePositions.end())
	{
		md.m_garrisonablePositions.push_front(*it);
		it++;
	}

	md.m_bridgeSpans = mdr.m_bridgeSpans;

	// Unpack the flat (i ascending, j ascending) pair list back into the
	// symmetric matrix. Unwritten pairs stay 0 ("not computed").
	{
		Int si, sj, k = 0;
		for (si = 0; si < MAX_SLOTS; ++si)
		{
			for (sj = si + 1; sj < MAX_SLOTS; ++sj)
			{
				md.m_startSpotPathDist[si][sj] = mdr.m_startSpotPathDist[k];
				md.m_startSpotPathDist[sj][si] = mdr.m_startSpotPathDist[k];
				++k;
			}
		}
	}

	if(TheMapCache && !md.m_displayName.isEmpty())
	{
		AsciiString lowerName = name;
		lowerName.toLower();
		md.m_fileName = lowerName;
//		DEBUG_LOG(("INI::parseMapCacheDefinition - adding %s to map cache", lowerName.str()));
		(*TheMapCache)[lowerName] = md;
	}
}

