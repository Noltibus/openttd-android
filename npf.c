#include "stdafx.h"
#include "ttd.h"
#include "debug.h"
#include "npf.h"
#include "aystar.h"
#include "macros.h"
#include "pathfind.h"
#include "station.h"
#include "tile.h"
#include "depot.h"

AyStar _train_find_station;
AyStar _train_find_depot;
AyStar _road_find_station;
AyStar _road_find_depot;
AyStar _npf_aystar;

/* Maps a trackdir to the bit that stores its status in the map arrays, in the
 * direction along with the trackdir */
const byte _signal_along_trackdir[14] = {
	0x80, 0x80, 0x80, 0x20, 0x40, 0x10, 0, 0,
	0x40, 0x40, 0x40, 0x10, 0x80, 0x20
};

/* Maps a trackdir to the bit that stores its status in the map arrays, in the
 * direction against the trackdir */
const byte _signal_against_trackdir[14] = {
	0x40, 0x40, 0x40, 0x10, 0x80, 0x20, 0, 0,
	0x80, 0x80, 0x80, 0x20, 0x40, 0x10
};

/* Maps a trackdir to the trackdirs that can be reached from it (ie, when
 * entering the next tile */
const uint16 _trackdir_reaches_trackdirs[14] = {
0x1009, 0x0016, 0x1009, 0x0016, 0x0520, 0x0016, 0, 0,
0x0520, 0x2A00, 0x2A00, 0x0520, 0x2A00, 0x1009
};

/* Maps a trackdir to all trackdirs that make 90 deg turns with it. */
const uint16 _trackdir_crosses_trackdirs[14] = {
0x0202, 0x0101, 0x3030, 0x3030, 0x0C0C, 0x0C0C, 0, 0,
0x0202, 0x0101, 0x3030, 0x3030, 0x0C0C, 0x0C0C
};

/* Maps a track to all tracks that make 90 deg turns with it. */
const byte _track_crosses_tracks[6] = {
	0x2, /* Track 1 -> Track 2 */
	0x1, /* Track 2 -> Track 1 */
	0x30, /* Upper -> Left | Right */
	0x30, /* Lower -> Left | Right */
	0x0C, /* Left -> Upper | Lower */
	0x0C, /* Right -> Upper | Lower */
};

/* Maps a trackdir to the (4-way) direction the tile is exited when following
 * that trackdir */
const byte _trackdir_to_exitdir[14] = {
	0,1,0,1,2,1, 0,0,
	2,3,3,2,3,0,
};

const byte _track_exitdir_to_trackdir[6][4] = {
	{0,    0xff, 8,    0xff},
	{0xff, 1,    0xff, 9},
	{2,    0xff, 0xff, 10},
	{0xff, 3,    11,   0xf},
	{0xff, 0xff, 4,    12},
	{13,   5,    0xff, 0xff}
};

const byte _track_direction_to_trackdir[6][8] = {
	{0xff, 0,    0xff, 0xff, 0xff, 8,    0xff, 0xff},
	{0xff, 0xff, 0xff, 1,    0xff, 0xff, 0xff, 9},
	{0xff, 0xff, 2,    0xff, 0xff, 0xff, 10,   0xff},
	{0xff, 0xff, 3,    0xff, 0xff, 0xff, 11,   0xff},
	{12,   0xff, 0xff, 0xff, 4,    0xff, 0xff, 0xff},
	{13,   0xff, 0xff, 0xff, 5,    0xff, 0xff, 0xff}
};

const byte _dir_to_diag_trackdir[4] = {
	0, 1, 8, 9,
};

const byte _reverse_dir[4] = {
	2, 3, 0, 1
};

const byte _reverse_trackdir[14] = {
	8, 9, 10, 11, 12, 13, 0xFF, 0xFF,
	0, 1, 2,  3,  4,  5
};

/* The cost of each trackdir. A diagonal piece is the full NPF_TILE_LENGTH,
 * the shorter piece is sqrt(2)/2*NPF_TILE_LENGTH =~ 0.7071
 */
#define NPF_STRAIGHT_LENGTH (uint)(NPF_TILE_LENGTH * 0.7071)
static const uint _trackdir_length[14] = {
NPF_TILE_LENGTH, NPF_TILE_LENGTH, NPF_STRAIGHT_LENGTH, NPF_STRAIGHT_LENGTH, NPF_STRAIGHT_LENGTH, NPF_STRAIGHT_LENGTH, 0, 0,
NPF_TILE_LENGTH, NPF_TILE_LENGTH, NPF_STRAIGHT_LENGTH, NPF_STRAIGHT_LENGTH, NPF_STRAIGHT_LENGTH, NPF_STRAIGHT_LENGTH
};

uint NTPHash(uint key1, uint key2)
{
	return PATHFIND_HASH_TILE(key1);
}

int32 NPFCalcZero(AyStar* as, AyStarNode* current, OpenListNode* parent) {
	return 0;
}

/* Calcs the heuristic to the target station (using NPFFindStationOrTileData). After
 * will save the heuristic into NPFFoundTargetData if it is the smallest until
 * now. It will then also save AyStarNode.user_data[NPF_TRACKDIR_CHOICE] in
 * best_trackdir
 */
int32 NPFCalcStationOrTileHeuristic(AyStar* as, AyStarNode* current, OpenListNode* parent) {
	NPFFindStationOrTileData* fstd = (NPFFindStationOrTileData*)as->user_target;
	NPFFoundTargetData* ftd = (NPFFoundTargetData*)as->user_path;
	TileIndex from = current->tile;
	TileIndex to = fstd->dest_coords;
	uint dist = DistanceManhattan(from, to) * NPF_TILE_LENGTH;

	if (dist < ftd->best_bird_dist) {
		ftd->best_bird_dist = dist;
		ftd->best_trackdir = current->user_data[NPF_TRACKDIR_CHOICE];
	}
	#ifdef NPF_DEBUG
	debug("Calculating H for: (%d, %d). Result: %d", TileX(current->tile), TileY(current->tile), dist);
	#endif
	return dist;
}

/* Fills AyStarNode.user_data[NPF_TRACKDIRCHOICE] with the chosen direction to
 * get here, either getting it from the current choice or from the parent's
 * choice */
void NPFFillTrackdirChoice(AyStarNode* current, OpenListNode* parent)
{
	if (parent->path.parent == NULL) {
		byte trackdir = current->direction;
		/* This is a first order decision, so we'd better save the
		 * direction we chose */
		current->user_data[NPF_TRACKDIR_CHOICE] = trackdir;
		#ifdef NPF_DEBUG
		debug("Saving trackdir: %#x", trackdir);
		#endif
	} else {
		/* We've already made the decision, so just save our parent's
		 * decision */
		current->user_data[NPF_TRACKDIR_CHOICE] = parent->path.node.user_data[NPF_TRACKDIR_CHOICE];
	}

}

/* Will return the cost of the tunnel. If it is an entry, it will return the
 * cost of that tile. If the tile is an exit, it will return the tunnel length
 * including the exit tile. Requires that this is a Tunnel tile */
uint NPFTunnelCost(AyStarNode* current) {
	byte exitdir = _trackdir_to_exitdir[current->direction];
	TileIndex tile = current->tile;
	if ( (uint)(_map5[tile] & 3) == _reverse_dir[exitdir]) {
		/* We just popped out if this tunnel, since were
		 * facing the tunnel exit */
		FindLengthOfTunnelResult flotr;
		flotr = FindLengthOfTunnel(tile, _reverse_dir[exitdir]);
		return flotr.length * NPF_TILE_LENGTH;
		//TODO: Penalty for tunnels?
	} else {
		/* We are entering the tunnel, the enter tile is just a
		 * straight track */
		return NPF_TILE_LENGTH;
	}
}

uint NPFSlopeCost(AyStarNode* current) {
	TileIndex next = current->tile + TileOffsByDir(_trackdir_to_exitdir[current->direction]);
	if (GetTileZ(next) > GetTileZ(current->tile)) {
		/* Slope up */
		return _patches.npf_rail_slope_penalty;
	}
	return 0;
	/* Should we give a bonus for slope down? Probably not, we
	 * could just substract that bonus from the penalty, because
	 * there is only one level of steepness... */
}

void NPFMarkTile(TileIndex tile) {
	switch(GetTileType(tile)) {
		case MP_RAILWAY:
		case MP_STREET:
			/* DEBUG: mark visited tiles by mowing the grass under them
			 * ;-) */
			_map2[tile] &= ~15;
			MarkTileDirtyByTile(tile);
			break;
		default:
			break;
	}
}

int32 NPFWaterPathCost(AyStar* as, AyStarNode* current, OpenListNode* parent) {
	//TileIndex tile = current->tile;
	int32 cost = 0;
	byte trackdir = current->direction;

	cost = _trackdir_length[trackdir]; /* Should be different for diagonal tracks */

	/* TODO Penalties? */

	return cost;
}

/* Determine the cost of this node, for road tracks */
int32 NPFRoadPathCost(AyStar* as, AyStarNode* current, OpenListNode* parent) {
	TileIndex tile = current->tile;
	int32 cost = 0;
	/* Determine base length */
	switch (GetTileType(tile)) {
		case MP_TUNNELBRIDGE:
			if ((_map5[tile] & 0xF0)==0) {
				cost = NPFTunnelCost(current);
				break;
			}
			/* Fall through if above if is false, it is a bridge
			 * then. We treat that as ordinary rail */
		case MP_STREET:
			cost = NPF_TILE_LENGTH;
			break;
		default:
			break;
	}

	/* Determine extra costs */

	/* Check for slope */
	cost += NPFSlopeCost(current);

	/* Check for turns */
	//TODO

	#ifdef NPF_MARKROUTE
	NPFMarkTile(tile);
	#endif
	#ifdef NPF_DEBUG
	debug("Calculating G for: (%d, %d). Result: %d", TileX(current->tile), TileY(current->tile), cost);
	#endif
	return cost;
}


/* Determine the cost of this node, for railway tracks */
int32 NPFRailPathCost(AyStar* as, AyStarNode* current, OpenListNode* parent) {
	TileIndex tile = current->tile;
	byte trackdir = current->direction;
	int32 cost = 0;
	/* Determine base length */
	switch (GetTileType(tile)) {
		case MP_TUNNELBRIDGE:
			if ((_map5[tile] & 0xF0)==0) {
				cost = NPFTunnelCost(current);
				break;
			}
			/* Fall through if above if is false, it is a bridge
			 * then. We treat that as ordinary rail */
		case MP_RAILWAY:
			cost = _trackdir_length[trackdir]; /* Should be different for diagonal tracks */
			break;
		case MP_STREET: /* Railway crossing */
			cost = NPF_TILE_LENGTH;
			break;
		default:
			break;
	}

	/* Determine extra costs */

	/* Ordinary track with signals */
	if (IsTileType(tile, MP_RAILWAY) && (_map5[tile] & 0xC0) == 0x40) {
		if ((_map2[tile] & _signal_along_trackdir[trackdir]) == 0) {
			/* Signal facing us is red */
			if (!(current->user_data[NPF_NODE_FLAGS] & NPF_FLAG_SEEN_SIGNAL)) {
				/* Penalize the first signal we
				 * encounter, if it is red */
				cost += _patches.npf_rail_firstred_penalty;
			}
		}
		current->user_data[NPF_NODE_FLAGS] |= NPF_FLAG_SEEN_SIGNAL;
	}

	/* Check for slope */
	cost += NPFSlopeCost(current);

	/* Check for turns */
	//TODO

	/* Check for occupied track */
	//TODO

	/* Check for station tiles */
	if (IsTileType(tile, MP_STATION)) {
		/* We give a station tile a penalty. Logically we would only
		 * want to give station tiles that are not our destination
		 * this penalty. This would discourage trains to drive through
		 * busy stations. But, we can just give any station tile a
		 * penalty, because every possible route will get this penalty
		 * exactly once, on its end tile (if it's a station) and it
		 * will therefore not make a difference. */
		cost += _patches.npf_rail_station_penalty;
	}

	#ifdef NPF_MARKROUTE
	NPFMarkTile(tile);
	#endif
	#ifdef NPF_DEBUG
	debug("Calculating G for: (%d, %d). Result: %d", TileX(current->tile), TileY(current->tile), cost);
	#endif
	return cost;
}

/* Will find any depot */
int32 NPFFindDepot(AyStar* as, OpenListNode *current) {
	TileIndex tile = current->path.node.tile;
	bool isDepot;
	switch(GetTileType(tile)) {
		case MP_RAILWAY:
			/* Check if this is a rail depot */
			isDepot = IsTrainDepotTile(tile);
			break;
		case MP_STREET:
			/* Check if this is a road depot */
			isDepot = IsRoadDepotTile(tile);
			break;
		case MP_WATER:
			isDepot = IsShipDepotTile(tile);
			break;
		default:
			isDepot = false;
			break;
	}
	if (isDepot)
		return AYSTAR_FOUND_END_NODE;
	else
		return AYSTAR_DONE;
}

/* Will find a station identified using the NPFFindStationOrTileData */
int32 NPFFindStationOrTile(AyStar* as, OpenListNode *current) {
	/* If GetNeighbours said we could get here, we assume the station type
	 * is correct */
	NPFFindStationOrTileData* fstd = (NPFFindStationOrTileData*)as->user_target;
	TileIndex tile = current->path.node.tile;
	if (	(fstd->station_index == -1 && tile == fstd->dest_coords) || /* We've found the tile, or */
		(IsTileType(tile, MP_STATION) && _map2[tile] == fstd->station_index) /* the station */
	   )
			return AYSTAR_FOUND_END_NODE;
	else
		return AYSTAR_DONE;
}

/* To be called when current contains the (shortest route to) the target node.
 * Will fill the contents of the NPFFoundTargetData using
 * AyStarNode[NPF_TRACKDIR_CHOICE].
 */
void NPFSaveTargetData(AyStar* as, OpenListNode* current) {
	NPFFoundTargetData* ftd = (NPFFoundTargetData*)as->user_path;
	ftd->best_trackdir = current->path.node.user_data[NPF_TRACKDIR_CHOICE];
	ftd->best_path_dist = current->g;
	ftd->best_bird_dist = 0;
	ftd->node = current->path.node;
}

/* Will just follow the results of GetTileTrackStatus concerning where we can
 * go and where not. Uses AyStar.user_data[NPF_TYPE] as the transport type and
 * an argument to GetTileTrackStatus. Will skip tunnels, meaning that the
 * entry and exit are neighbours. Will fill AyStarNode.user_data[NPF_TRACKDIR_CHOICE] with an
 * appropriate value, and copy AyStarNode.user_data[NPF_NODE_FLAGS] from the
 * parent */
void NPFFollowTrack(AyStar* aystar, OpenListNode* current) {
	byte src_trackdir = current->path.node.direction;
	TileIndex src_tile = current->path.node.tile;
	byte src_exitdir = _trackdir_to_exitdir[src_trackdir];
	FindLengthOfTunnelResult flotr;
	TileIndex dst_tile;
	int i = 0;
	uint trackdirs, ts;
	TransportType type = aystar->user_data[NPF_TYPE];
	/* Initialize to 0, so we can jump out (return) somewhere an have no neighbours */
	aystar->num_neighbours = 0;
	#ifdef NPF_DEBUG
	debug("Expanding: (%d, %d, %d) [%d]", TileX(src_tile), TileY(src_tile), src_trackdir, src_tile);
	#endif

	/* Find dest tile */
	if (IsTileType(src_tile, MP_TUNNELBRIDGE) && (_map5[src_tile] & 0xF0)==0 && (_map5[src_tile] & 3) == src_exitdir) {
		/* This is a tunnel. We know this tunnel is our type,
		 * otherwise we wouldn't have got here. It is also facing us,
		 * so we should skip it's body */
		flotr = FindLengthOfTunnel(src_tile, src_exitdir);
		dst_tile = flotr.tile;
	} else {
		if (
			(type == TRANSPORT_ROAD && IsRoadStationTile(src_tile))
			|| (type == TRANSPORT_ROAD && IsRoadDepotTile(src_tile))
			|| (type == TRANSPORT_RAIL && IsTrainDepotTile(src_tile))
			){
			/* This is a road station or a train or road depot. We can enter and exit
			 * those from one side only. Trackdirs don't support that (yet), so we'll
			 * do this here. */

			byte exitdir;
			/* Find out the exit direction first */
			if (IsRoadStationTile(src_tile))
				exitdir = GetRoadStationDir(src_tile);
			else /* Train or road depot. Direction is stored the same for both, in map5 */
				exitdir = _map5[src_tile] & 3; /* Extract the direction from the map */

			/* Let's see if were headed the right way */
			if (src_trackdir != _dir_to_diag_trackdir[exitdir])
				/* Not going out of the station/depot through the exit, but the back. No
				 * neighbours then. */
				return;
		}
		/* This a normal tile, a bridge, a tunnel exit, etc. */
		dst_tile = AddTileIndexDiffCWrap(src_tile, TileIndexDiffCByDir(_trackdir_to_exitdir[src_trackdir]));
		if (dst_tile == INVALID_TILE) {
			/* We reached the border of the map */
			/* TODO Nicer control flow for this */
			return;
		}
	}

	// TODO: check correct rail type (mono, maglev, etc)
	// TODO: check tile owner

	/* Determine available tracks */
	if (type == TRANSPORT_ROAD && (IsRoadStationTile(dst_tile) || IsRoadDepotTile(dst_tile))){
		byte exitdir;
		/* Road stations and depots return 0 on GTTS, so we have to do this by hand... */
			if (IsRoadStationTile(dst_tile))
				exitdir = GetRoadStationDir(dst_tile);
			else /* Road depot */
				exitdir = _map5[dst_tile] & 3; /* Extract the direction from the map */
			ts = (1 << _dir_to_diag_trackdir[exitdir]) |
					(1 << _dir_to_diag_trackdir[_reverse_dir[exitdir]]);
					/* Find the trackdirs that are available for a station with this orientation. They are in both directions */
	} else {
		ts = GetTileTrackStatus(dst_tile, type);
	}
	trackdirs = ts & 0x3F3F; /* Filter out signal status and the unused bits */

	#ifdef NPF_DEBUG
	debug("Next node: (%d, %d) [%d], possible trackdirs: %#x", TileX(dst_tile), TileY(dst_tile), dst_tile, trackdirs);
	#endif
	/* Select only trackdirs we can reach from our current trackdir */
	trackdirs &= _trackdir_reaches_trackdirs[src_trackdir];
	if (_patches.forbid_90_deg && (type == TRANSPORT_RAIL || type == TRANSPORT_WATER)) /* Filter out trackdirs that would make 90 deg turns for trains */
		trackdirs &= ~_trackdir_crosses_trackdirs[src_trackdir];
	#ifdef NPF_DEBUG
	debug("After filtering: (%d, %d), possible trackdirs: %#x", TileX(dst_tile), TileY(dst_tile), trackdirs);
	#endif

	/* Enumerate possible track */
	while (trackdirs != 0) {
		byte dst_trackdir;
		dst_trackdir =  FindFirstBit2x64(trackdirs);
		trackdirs = KillFirstBit2x64(trackdirs);
		#ifdef NPF_DEBUG
		debug("Expanded into trackdir: %d, remaining trackdirs: %#x", dst_trackdir, trackdirs);
		#endif

		/* Check for oneway signal against us */
		if (IsTileType(dst_tile, MP_RAILWAY) && (_map5[dst_tile]&0xC0) == 0x40) {
			// the tile has a signal
			byte signal_present = _map3_lo[dst_tile];
			if (!(signal_present & _signal_along_trackdir[dst_trackdir])) {
				// if one way signal not pointing towards us, stop going in this direction.
				if (signal_present & _signal_against_trackdir[dst_trackdir])
					break;
			}
		}
		{
			/* We've found ourselves a neighbour :-) */
			AyStarNode* neighbour = &aystar->neighbours[i];
			neighbour->tile = dst_tile;
			neighbour->direction = dst_trackdir;
			/* Save user data */
			neighbour->user_data[NPF_NODE_FLAGS] = current->path.node.user_data[NPF_NODE_FLAGS];
			NPFFillTrackdirChoice(neighbour, current);
		}
		i++;
	}
	aystar->num_neighbours = i;
}

/*
 * Plan a route to the specified target (which is checked by target_proc),
 * from start1 and if not NULL, from start2 as well. The type of transport we
 * are checking is in type.
 * When we are looking for one specific target (optionally multiple tiles), we
 * should use a good heuristic to perform aystar search. When we search for
 * multiple targets that are spread around, we should perform a breadth first
 * search by specifiying CalcZero as our heuristic.
 */
NPFFoundTargetData NPFRouteInternal(AyStarNode* start1, AyStarNode* start2, NPFFindStationOrTileData* target, AyStar_EndNodeCheck target_proc, AyStar_CalculateH heuristic_proc, TransportType type) {
	int r;
	NPFFoundTargetData result;

	/* Initialize procs */
	_npf_aystar.CalculateH = heuristic_proc;
	_npf_aystar.EndNodeCheck = target_proc;
	_npf_aystar.FoundEndNode = NPFSaveTargetData;
	_npf_aystar.GetNeighbours = NPFFollowTrack;
	if (type == TRANSPORT_RAIL)
		_npf_aystar.CalculateG = NPFRailPathCost;
	else if (type == TRANSPORT_ROAD)
		_npf_aystar.CalculateG = NPFRoadPathCost;
	else if (type == TRANSPORT_WATER)
		_npf_aystar.CalculateG = NPFWaterPathCost;
	else
		assert(0);

	/* Initialize Start Node(s) */
	start1->user_data[NPF_TRACKDIR_CHOICE] = 0xff;
	start1->user_data[NPF_NODE_FLAGS] = 0;
	_npf_aystar.addstart(&_npf_aystar, start1);
	if (start2) {
		start2->user_data[NPF_TRACKDIR_CHOICE] = 0xff;
		start2->user_data[NPF_NODE_FLAGS] = NPF_FLAG_REVERSE;
		_npf_aystar.addstart(&_npf_aystar, start2);
	}

	/* Initialize result */
	result.best_bird_dist = (uint)-1;
	result.best_path_dist = (uint)-1;
	result.best_trackdir = 0xff;
	_npf_aystar.user_path = &result;

	/* Initialize target */
	_npf_aystar.user_target = target;

	/* Initialize user_data */
	_npf_aystar.user_data[NPF_TYPE] = type;

	/* GO! */
	r = AyStarMain_Main(&_npf_aystar);
	assert(r != AYSTAR_STILL_BUSY);

	if (result.best_bird_dist != 0) {
		if (target) {
			DEBUG(misc, 1) ("NPF: Could not find route to 0x%x from 0x%x.", target->dest_coords, start1->tile);
		} else {
			/* Assumption: target == NULL, so we are looking for a depot */
			DEBUG(misc, 1) ("NPF: Could not find route to a depot from 0x%x.", start1->tile);
		}

	}
	return result;
}

NPFFoundTargetData NPFRouteToStationOrTileTwoWay(TileIndex tile1, byte trackdir1, TileIndex tile2, byte trackdir2, NPFFindStationOrTileData* target, TransportType type) {
	AyStarNode start1;
	AyStarNode start2;

	start1.tile = tile1;
	start2.tile = tile2;
	start1.direction = trackdir1;
	start2.direction = trackdir2;

	return NPFRouteInternal(&start1, &start2, target, NPFFindStationOrTile, NPFCalcStationOrTileHeuristic, type);
}

NPFFoundTargetData NPFRouteToStationOrTile(TileIndex tile, byte trackdir, NPFFindStationOrTileData* target, TransportType type) {
	AyStarNode start;

	assert(tile != 0);

	start.tile = tile;
	start.direction = trackdir;
	/* We set this in case the target is also the start tile, we will just
	 * return a not found then */
	start.user_data[NPF_TRACKDIR_CHOICE] = 0xff;

	return NPFRouteInternal(&start, NULL, target, NPFFindStationOrTile, NPFCalcStationOrTileHeuristic, type);
}

NPFFoundTargetData NPFRouteToDepotBreadthFirst(TileIndex tile, byte trackdir, TransportType type) {
	AyStarNode start;

	start.tile = tile;
	start.direction = trackdir;
	/* We set this in case the target is also the start tile, we will just
	 * return a not found then */
	start.user_data[NPF_TRACKDIR_CHOICE] = 0xff;

	/* perform a breadth first search. Target is NULL,
	 * since we are just looking for any depot...*/
	return NPFRouteInternal(&start, NULL, NULL, NPFFindDepot, NPFCalcZero, type);
}

NPFFoundTargetData NPFRouteToDepotTrialError(TileIndex tile, byte trackdir, TransportType type) {
	/* Okay, what we're gonna do. First, we look at all depots, calculate
	 * the manhatten distance to get to each depot. We then sort them by
	 * distance. We start by trying to plan a route to the closest, then
	 * the next closest, etc. We stop when the best route we have found so
	 * far, is shorter than the manhattan distance. This will obviously
	 * always find the closest depot. It will probably be most efficient
	 * for ships, since the heuristic will not be to far off then. I hope.
	 */
	Queue depots;
	TileType tiletype = 0;
	int r;
	NPFFoundTargetData best_result;
	NPFFoundTargetData result;
	NPFFindStationOrTileData target;
	AyStarNode start;
	Depot* current;
	Depot *depot;


	/* This is a little ugly, but it works :-S */
	if (type == TRANSPORT_ROAD)
		tiletype = MP_STREET;
	else if (type == TRANSPORT_RAIL)
		tiletype = MP_RAILWAY;
	else if (type == TRANSPORT_WATER)
		tiletype = MP_WATER;
	else
		assert(0);

	init_InsSort(&depots);
	/* Okay, let's find all depots that we can use first */
	FOR_ALL_DEPOTS(depot) {
		if (IsTileType(depot->xy, tiletype))
			depots.push(&depots, depot, DistanceManhattan(tile, depot->xy));
	}

	/* Now, let's initialise the aystar */

	/* Initialize procs */
	_npf_aystar.CalculateH = NPFCalcStationOrTileHeuristic;
	_npf_aystar.EndNodeCheck = NPFFindStationOrTile;
	_npf_aystar.FoundEndNode = NPFSaveTargetData;
	_npf_aystar.GetNeighbours = NPFFollowTrack;
	if (type == TRANSPORT_RAIL)
		_npf_aystar.CalculateG = NPFRailPathCost;
	else if (type == TRANSPORT_ROAD)
		_npf_aystar.CalculateG = NPFRoadPathCost;
	else if (type == TRANSPORT_WATER)
		_npf_aystar.CalculateG = NPFWaterPathCost;
	else
		assert(0);

	/* Initialize target */
	target.station_index = -1; /* We will initialize dest_coords inside the loop below */
	_npf_aystar.user_target = &target;

	/* Initialize user_data */
	_npf_aystar.user_data[NPF_TYPE] = type;

	/* Initialize Start Node */
	start.tile = tile;
	start.direction = trackdir; /* We will initialize user_data inside the loop below */

	/* Initialize Result */
	_npf_aystar.user_path = &result;
	best_result.best_path_dist = (uint)-1;

	/* Just iterate the depots in order of increasing distance */
	while ((current = depots.pop(&depots))) {
		/* Check to see if we already have a path shorter than this
		 * depot's manhattan distance. Hack: We call DistanceManhattan
		 * again, we should probably modify the queue to give us that
		 * value... */
		if ( DistanceManhattan(tile, current->xy * NPF_TILE_LENGTH) > best_result.best_path_dist)
			break;

		/* Initialize Start Node */
		/* We set this in case the target is also the start tile, we will just
		 * return a not found then */
		start.user_data[NPF_TRACKDIR_CHOICE] = 0xff;
		start.user_data[NPF_NODE_FLAGS] = 0;
		_npf_aystar.addstart(&_npf_aystar, &start);

		/* Initialize result */
		result.best_bird_dist = (uint)-1;
		result.best_path_dist = (uint)-1;
		result.best_trackdir = 0xff;

		/* Initialize target */
		target.dest_coords = current->xy;

		/* GO! */
		r = AyStarMain_Main(&_npf_aystar);
		assert(r != AYSTAR_STILL_BUSY);

		/* This depot is closer */
		if (result.best_path_dist < best_result.best_path_dist)
			best_result = result;
	}
	if (result.best_bird_dist != 0) {
		DEBUG(misc, 1) ("NPF: Could not find route to any depot from 0x%x.", tile);
	}
	return best_result;
}

void InitializeNPF(void)
{
		init_AyStar(&_npf_aystar, NTPHash, 1024);
		_npf_aystar.loops_per_tick = 0;
		_npf_aystar.max_path_cost = 0;
		_npf_aystar.max_search_nodes = 0;
		/*
		init_AyStar(&_train_find_station, NTPHash, 1024);
		init_AyStar(&_train_find_depot, NTPHash, 1024);
		init_AyStar(&_road_find_station, NTPHash, 1024);
		init_AyStar(&_road_find_depot, NTPHash, 1024);

		_train_find_station.loops_per_tick = 0;
		_train_find_depot.loops_per_tick = 0;
		_road_find_station.loops_per_tick = 0;
		_road_find_depot.loops_per_tick = 0;

		_train_find_station.max_path_cost = 0;
		_train_find_depot.max_path_cost = 0;
		_road_find_station.max_path_cost = 0;
		_road_find_depot.max_path_cost = 0;

		_train_find_station.max_search_nodes = 0;
		_train_find_depot.max_search_nodes = 0;
		_road_find_station.max_search_nodes = 0;
		_road_find_depot.max_search_nodes = 0;

		_train_find_station.CalculateG = NPFRailPathCost;
		_train_find_depot.CalculateG = NPFRailPathCost;
		_road_find_station.CalculateG = NPFRoadPathCost;
		_road_find_depot.CalculateG = NPFRoadPathCost;

		_train_find_station.CalculateH = NPFCalcStationHeuristic;
		_train_find_depot.CalculateH = NPFCalcStationHeuristic;
		_road_find_station.CalculateH = NPFCalcStationHeuristic;
		_road_find_depot.CalculateH = NPFCalcStationHeuristic;

		_train_find_station.EndNodeCheck = NPFFindStationOrTile;
		_train_find_depot.EndNodeCheck = NPFFindStationOrTile;
		_road_find_station.EndNodeCheck = NPFFindStationOrTile;
		_road_find_depot.EndNodeCheck = NPFFindStationOrTile;

		_train_find_station.FoundEndNode = NPFSaveTargetData;
		_train_find_depot.FoundEndNode = NPFSaveTargetData;
		_road_find_station.FoundEndNode = NPFSaveTargetData;
		_road_find_depot.FoundEndNode = NPFSaveTargetData;

		_train_find_station.GetNeighbours = NPFFollowTrack;
		_train_find_depot.GetNeighbours = NPFFollowTrack;
		_road_find_station.GetNeighbours = NPFFollowTrack;
		_road_find_depot.GetNeighbours = NPFFollowTrack;
		*/
}

void NPFFillWithOrderData(NPFFindStationOrTileData* fstd, Vehicle* v) {
	/* Ships don't really reach their stations, but the tile in front. So don't
	 * save the station id for ships. For roadvehs we don't store it either,
	 * because multistop depends on vehicles actually reaching the exact
	 * dest_tile, not just any stop of that station.
	 * So only for train orders to stations we fill fstd->station_index, for all
	 * others only dest_coords */
	if ((v->current_order.type) == OT_GOTO_STATION && v->type == VEH_Train) {
		const Station* st = GetStation(v->current_order.station);
		TileIndexDiffC center = {st->trainst_w/2, st->trainst_h/2};
		fstd->station_index = v->current_order.station;
		/* Let's take the center of the station as our target tile for trains */
		fstd->dest_coords = TILE_ADD(st->train_tile, ToTileIndexDiff(center));
	} else {
		fstd->dest_coords = v->dest_tile;
		fstd->station_index = -1;
	}
}
