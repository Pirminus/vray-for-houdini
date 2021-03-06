//
// Copyright (c) 2015-2017, Chaos Software Ltd
//
// V-Ray For Houdini
//
// ACCESSIBLE SOURCE CODE WITHOUT DISTRIBUTION OF MODIFICATION LICENSE
//
// Full license text: https://github.com/ChaosGroup/vray-for-houdini/blob/master/LICENSE
//

#include "vop_TexGradRamp.h"
#include "vfh_tex_utils.h"


using namespace VRayForHoudini;


void VOP::TexGradRamp::setPluginType()
{
	pluginType = VRayPluginType::TEXTURE;
	pluginID   = "TexGradRamp";
}


OP::VRayNode::PluginResult VOP::TexGradRamp::asPluginDesc(Attrs::PluginDesc &pluginDesc, VRayExporter &exporter, ExportContext *parentContext)
{
	return OP::VRayNode::PluginResultContinue;
}
