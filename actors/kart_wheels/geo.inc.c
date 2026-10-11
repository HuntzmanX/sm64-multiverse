#include "src/game/envfx_snow.h"

const GeoLayout kart_wheels_geo[] = {
	GEO_NODE_START(),
	GEO_OPEN_NODE(),
		GEO_SCALE(0x00, 147456),
		GEO_OPEN_NODE(),
			GEO_DISPLAY_LIST(LAYER_OPAQUE, kart_wheels_polygon0_mesh_layer_1_with_revert),
		GEO_CLOSE_NODE(),
	GEO_CLOSE_NODE(),
	GEO_END(),
};
