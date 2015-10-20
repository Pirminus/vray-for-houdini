//
// Copyright (c) 2015, Chaos Software Ltd
//
// V-Ray For Houdini
//
// ACCESSIBLE SOURCE CODE WITHOUT DISTRIBUTION OF MODIFICATION LICENSE
//
// Full license text: https://github.com/ChaosGroup/vray-for-houdini/blob/master/LICENSE
//

#include "vfh_defines.h"
#include "vfh_exporter.h"
#include "vfh_prm_globals.h"
#include "vfh_prm_templates.h"
#include "vfh_tex_utils.h"

#include "obj/obj_node_base.h"
#include "vop/vop_node_base.h"
#include "vop/material/vop_mtl_def.h"
#include "sop/sop_vrayproxy.h"
#include "sop/sop_vrayscene.h"

#include <OP/OP_Node.h>
#include <OP/OP_Bundle.h>
#include <ROP/ROP_Node.h>
#include <SHOP/SHOP_Node.h>
#include <SOP/SOP_Node.h>
#include <VOP/VOP_Node.h>

#include <OBJ/OBJ_Camera.h>
#include <OBJ/OBJ_Geometry.h>
#include <OBJ/OBJ_Node.h>
#include <OBJ/OBJ_Light.h>
#include <OBJ/OBJ_SubNet.h>

#include <RE/RE_Render.h>
#include <RE/RE_Window.h>

#include <OP/OP_Director.h>

#include <boost/bind.hpp>
#include <boost/format.hpp>


using namespace VRayForHoudini;


VRayExporter::ExporterInstances  VRayExporter::Instances;


std::string VRayExporter::getPluginName(OP_Node *op_node, const std::string &prefix, const std::string &suffix)
{
	static boost::format FmtPlugin("%s@%s|%s|%s");

	const std::string &pluginName = boost::str(FmtPlugin
											   % prefix
											   % op_node->getName().buffer()
											   % op_node->getParentNetwork()->getName().buffer()
											   % suffix);

	return pluginName;
}


std::string VRayExporter::getPluginName(OBJ_Node *obj_node)
{
	std::string pluginName;

	const OBJ_OBJECT_TYPE ob_type = obj_node->getObjectType();
	if (ob_type & OBJ_LIGHT) {
		static boost::format FmtLight("Light@%s");
		pluginName = boost::str(FmtLight
								% obj_node->getName().buffer());
	}
	else if (ob_type & OBJ_CAMERA) {
		static boost::format FmtCamera("Camera@%s");
		pluginName = boost::str(FmtCamera
								% obj_node->getName().buffer());
	}
	else if (ob_type == OBJ_GEOMETRY) {
		static boost::format FmtObject("Node@%s");
		pluginName = boost::str(FmtObject
								% obj_node->getName().buffer());
	}

	return pluginName;
}


VRay::Transform VRayExporter::Matrix4ToTransform(const UT_Matrix4D &m4, bool flip)
{
	VRay::Transform tm;
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			tm.matrix[i][j] = m4[i][j];
		}
		tm.offset[i] = m4[3][i];
	}

	if (flip) {
		VUtils::swap(tm.matrix[1], tm.matrix[2]);
	}

	return tm;
}


VRay::Transform VRayExporter::GetOBJTransform(OBJ_Node *obj_node, OP_Context &context, bool flip)
{
	UT_Matrix4D matrix;
	obj_node->getLocalToWorldTransform(context, matrix);

	return VRayExporter::Matrix4ToTransform(matrix, flip);
}


void VRayExporter::GetOBJTransform(OBJ_Node *obj_node, OP_Context &context, float tm[4][4])
{
	UT_Matrix4D matrix;
	obj_node->getLocalToWorldTransform(context, matrix);

	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			tm[i][j] = matrix[i][j];
		}
	}
}


void VRayExporter::TransformToMatrix4(const VUtils::TraceTransform &tm, UT_Matrix4 &m)
{
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			m(i,j) = tm.m[i][j];
		}
		m(3,i) = tm.offs[i];
	}
}


OBJ_Node *VRayExporter::GetCamera(OP_Node *rop)
{
	OBJ_Node *camera = nullptr;

	UT_String camera_path;
	rop->evalString(camera_path, "render_camera", 0, 0.0);
	if (NOT(camera_path.equal(""))) {
		OP_Node *node = OPgetDirector()->findNode(camera_path.buffer());
		if (node) {
			camera = node->castToOBJNode();
		}
	}

	return camera;
}


OP_Node* VRayExporter::FindChildNodeByType(OP_Node *op_node, const std::string &op_type)
{
	OP_NodeList childNodes;
	op_node->getAllChildren(childNodes);

	for (auto childIt : childNodes) {
		const UT_String &opType = childIt->getOperator()->getName();
		if (opType.equal(op_type.c_str())) {
			return childIt;
		}
	}

	return nullptr;
}


void VRayExporter::setAttrValueFromOpNode(Attrs::PluginDesc &pluginDesc, const Parm::AttrDesc &attrDesc, OP_Node *opNode, const std::string &prefix)
{
	const std::string &parmName = prefix.empty()
								  ? attrDesc.attr
								  : boost::str(Parm::FmtPrefixManual % prefix % attrDesc.attr);

	if (Parm::isParmExist(*opNode, parmName)) {
		const fpreal &t = m_context.getTime();
#if 0
		PRINT_INFO("Setting: [%s] %s.%s (from %s.%s)",
				   pluginDesc.pluginID.c_str(),
				   pluginDesc.pluginName.c_str(), attrDesc.attr.c_str(),
				   opNode->getName().buffer(), parmName.c_str());
#endif
		Attrs::PluginAttr attr;
		attr.paramName = attrDesc.attr;

		if (attrDesc.value.type == Parm::eBool ||
			attrDesc.value.type == Parm::eInt  ||
			attrDesc.value.type == Parm::eTextureInt)
		{
			attr.paramType = Attrs::PluginAttr::AttrTypeInt;
			attr.paramValue.valInt = opNode->evalInt(parmName.c_str(), 0, t);
		}
		else if (attrDesc.value.type == Parm::eEnum) {
			const int menuIndex = opNode->evalInt(parmName.c_str(), 0, t);

			const Parm::EnumItem &enumItem = attrDesc.value.defEnumItems[menuIndex];
			if (enumItem.valueType == Parm::EnumItem::EnumValueInt) {
				attr.paramType = Attrs::PluginAttr::AttrTypeInt;
				attr.paramValue.valInt = enumItem.value;
			}
			else {
				attr.paramType = Attrs::PluginAttr::AttrTypeString;
				attr.paramValue.valString = enumItem.valueString;
			}
		}
		else if (attrDesc.value.type == Parm::eFloat ||
				 attrDesc.value.type == Parm::eTextureFloat) {
			attr.paramType = Attrs::PluginAttr::AttrTypeFloat;
			attr.paramValue.valFloat = (float)opNode->evalFloat(parmName.c_str(), 0, t);
		}
		else if (attrDesc.value.type == Parm::eColor  ||
				 attrDesc.value.type == Parm::eAColor ||
				 attrDesc.value.type == Parm::eTextureColor)
		{
			attr.paramType = Attrs::PluginAttr::AttrTypeColor;
			attr.paramValue.valVector[0] = (float)opNode->evalFloat(parmName.c_str(), 0, t);
			attr.paramValue.valVector[1] = (float)opNode->evalFloat(parmName.c_str(), 1, t);
			attr.paramValue.valVector[2] = (float)opNode->evalFloat(parmName.c_str(), 2, t);
			if (attrDesc.value.type != Parm::eColor) {
				attr.paramValue.valVector[3] = (float)opNode->evalFloat(parmName.c_str(), 3, t);
			}
		}
		else if (attrDesc.value.type == Parm::eString) {
			UT_String buf;
			opNode->evalString(buf, parmName.c_str(), 0, t);

			attr.paramType = Attrs::PluginAttr::AttrTypeString;
			attr.paramValue.valString = buf.buffer();
		}
		else if (attrDesc.value.type > Parm::eManualExportStart && attrDesc.value.type < Parm::eManualExportEnd) {
			// These are fake params and must be handled manually
		}
		else if (attrDesc.value.type < Parm::ePlugin) {
			PRINT_ERROR("Unhandled param type: %s at %s [%i]",
						parmName.c_str(), opNode->getOperator()->getName().buffer(), attrDesc.value.type);
		}

		pluginDesc.addAttribute(attr);

	}
}


int VRayExporter::setAttrsFromOpNode(Attrs::PluginDesc &pluginDesc, OP_Node *opNode, const std::string &prefix)
{
	const Parm::VRayPluginInfo *pluginInfo = Parm::GetVRayPluginInfo(pluginDesc.pluginID);
	if (NOT(pluginInfo)) {
		PRINT_ERROR("Node \"%s\": Plugin \"%s\" description is not found!",
					opNode->getName().buffer(), pluginDesc.pluginID.c_str());
		return 1;
	}

	for (const auto &aIt : pluginInfo->attributes) {
		const std::string    &attrName = aIt.first;
		const Parm::AttrDesc &attrDesc = aIt.second;

		if (pluginDesc.contains(attrName)) {
			continue;
		}

		if (attrDesc.custom_handling) {
			continue;
		}

		// Check if attribute value has a socket and is linked
		//
		VRay::Plugin plugin_value = VRay::Plugin();
		for (const auto &inSockInfo : pluginInfo->inputs) {
			if (attrName == inSockInfo.name.getToken()) {
				plugin_value = exportConnectedVop(opNode, attrName.c_str());
				break;
			}
		}

		if (NOT(plugin_value)) {
			// Provide some default mapping for textures
			if (pluginInfo->pluginType == Parm::PluginTypeTexture && attrName == "uvwgen") {
				Attrs::PluginDesc uvwGen(VRayExporter::getPluginName(opNode, "Uvw"), "UVWGenObject");
				plugin_value = exportPlugin(uvwGen);
			}
		}

		if (NOT(plugin_value)) {
			if (attrDesc.value.type == Parm::eRamp) {
				Texture::exportRampAttribute(this, pluginDesc, opNode,
											 /* Houdini ramp attr */ attrDesc.attr,
											 /* V-Ray attr: colors */ attrDesc.value.defRamp.colors,
											 /* V-Ray attr: pos    */ attrDesc.value.defRamp.positions,
											 /* V-Ray attr: interp */ attrDesc.value.defRamp.interpolations,
											 /* As color list not plugin */ true);
			}
			else if (attrDesc.value.type == Parm::eCurve) {
				VRay::IntList    interpolations;
				VRay::FloatList  positions;
				VRay::FloatList  values;
				VRay::FloatList *valuesPtr = attrDesc.value.defCurve.values.empty()
											 ? nullptr
											 : &values;

				Texture::getCurveData(this, opNode,
									  /* Houdini curve attr */ attrDesc.attr,
									  /* V-Ray attr: interp */ interpolations,
									  /* V-Ray attr: x      */ positions,
									  /* V-Ray attr: y      */ valuesPtr,
									  /* Don't need handles */ false);

				pluginDesc.addAttribute(Attrs::PluginAttr(attrDesc.value.defCurve.interpolations, interpolations));
				pluginDesc.addAttribute(Attrs::PluginAttr(attrDesc.value.defCurve.positions,      positions));
				if (valuesPtr) {
					pluginDesc.addAttribute(Attrs::PluginAttr(attrDesc.value.defCurve.values,     values));
				}
			}
			else {
				setAttrValueFromOpNode(pluginDesc, attrDesc, opNode, prefix);
			}
		}
		else {
			PRINT_INFO("  Setting plugin value: %s = %s",
					   attrName.c_str(), plugin_value.getName().c_str());

			const Parm::SocketDesc *fromSocketInfo = getConnectedOutputType(opNode, attrName.c_str());

			if (fromSocketInfo &&
				fromSocketInfo->type >= Parm::ParmType::eOutputColor &&
				fromSocketInfo->type  < Parm::ParmType::eUnknown)
			{
				PRINT_INFO("    Using output: %s (\"%s\")",
						   fromSocketInfo->name.getToken(), fromSocketInfo->name.getLabel());
				pluginDesc.addAttribute(Attrs::PluginAttr(attrName, plugin_value, fromSocketInfo->name.getToken()));
			}
			else {
				pluginDesc.addAttribute(Attrs::PluginAttr(attrName, plugin_value));
			}
		}
	}

	return 0;
}


VRayExporter::VRayExporter():
	m_rop(nullptr)
{
	VRayExporter::Instances.insert(this);
}


VRayExporter::~VRayExporter()
{
	PRINT_WARN("~VRayExporter()");

	VRayExporter::Instances.erase(this);

	resetOpCallbacks();
}


int VRayExporter::init(int mode)
{
	m_renderer.init(false);
	m_renderer.resetObjects();
	m_renderer.setMode(mode);

	m_phxSimulations.clear();
	m_is_aborted = false;

	return 0;
}


void VRayExporter::RtCallbackView(OP_Node *caller, void *callee, OP_EventType type, void *data)
{
	VRayExporter *exporter = (VRayExporter*)callee;

	PRINT_INFO("RtCallbackView: %s from \"%s\"",
			   OPeventToString(type), caller->getName().buffer());

	if (type == OP_PARM_CHANGED ||
		type == OP_INPUT_CHANGED)
	{
		exporter->exportView();
	}
	else if (type == OP_NODE_PREDELETE) {
		exporter->delOpCallback(caller, VRayExporter::RtCallbackView);
	}
}


int VRayExporter::exportSettings(OP_Node *rop)
{
	for (const auto &sp : Parm::RenderSettingsPlugins) {
		const Parm::VRayPluginInfo *pluginInfo = Parm::GetVRayPluginInfo(sp);
		if (!pluginInfo) {
			PRINT_ERROR("Plugin \"%s\" description is not found!",
						sp.c_str());
		}
		else {
			Attrs::PluginDesc pluginDesc(sp, sp);
			setAttrsFromOpNode(pluginDesc, rop, boost::str(Parm::FmtPrefix % sp));
			exportPlugin(pluginDesc);
		}
	}

	Attrs::PluginDesc pluginDesc("settingsUnitsInfo", "SettingsUnitsInfo");
	pluginDesc.addAttribute(Attrs::PluginAttr("scene_upDir", VRay::Vector(0.0f, 1.0f, 0.0f)));
	exportPlugin(pluginDesc);

	return 0;
}


void VRayExporter::exportEnvironment(OP_Node *op_node)
{
	exportVop(op_node);
}


void VRayExporter::exportEffects(OP_Node *op_net)
{
	// Test simulation export
	// Add simulations from ROP
	OP_Node *sim_node = VRayExporter::FindChildNodeByType(op_net, "VRayNodePhxShaderSimVol");
	if (sim_node) {
		exportVop(sim_node);
	}
}


void VRayExporter::phxAddSimumation(VRay::Plugin sim)
{
	m_phxSimulations.push_back(VRay::Value(sim));
}


void VRayExporter::exportRenderChannels(OP_Node *op_node)
{
	exportVop(op_node);
}


OP_Input* VRayExporter::getConnectedInput(OP_Node *op_node, const std::string &inputName)
{
	const unsigned input_idx = op_node->getInputFromName(inputName.c_str());
	return op_node->getInputReferenceConst(input_idx);
}


OP_Node* VRayExporter::getConnectedNode(OP_Node *op_node, const std::string &inputName)
{
	OP_Input *input = getConnectedInput(op_node, inputName);
	if (input) {
		return input->getNode();
	}
	return nullptr;
}


const Parm::SocketDesc* VRayExporter::getConnectedOutputType(OP_Node *op_node, const std::string &inputName)
{
	OP_Node *connNode = getConnectedNode(op_node, inputName);
	if (connNode) {
		const UT_String &opType = connNode->getOperator()->getName();
		if (opType.startsWith("VRayNode")) {
			OP_Input *fromOutput = getConnectedInput(op_node, inputName);
			if (fromOutput) {
				VOP::NodeBase *vrayNode = static_cast<VOP::NodeBase*>(connNode);

				const Parm::VRayPluginInfo *pluginInfo = vrayNode->getVRayPluginInfo();

				const int &fromOutputIdx = fromOutput->getNodeOutputIndex();

				if (fromOutputIdx < pluginInfo->outputs.size()) {
					return &pluginInfo->outputs[fromOutputIdx];
				}
			}
		}
	}

	return nullptr;
}


VRay::Plugin VRayExporter::exportConnectedVop(OP_Node *op_node, const UT_String &inputName)
{
	const unsigned input_idx = op_node->getInputFromName(inputName);
	OP_Input *input = op_node->getInputReferenceConst(input_idx);
	if (input) {
		OP_Node *connNode = input->getNode();
		if (connNode) {
			return exportVop(connNode);
		}
	}

	return VRay::Plugin();
}


int VRayExporter::processAnimatedNode(OP_Node *op_node)
{
	int process = true;

	if (m_is_animation && (m_timeCurrent > m_timeStart)) {
		// TODO: Need to go through inputs...
		process = op_node->hasAnimatedParms();
	}

	return process;
}


void VRayExporter::RtCallbackVop(OP_Node *caller, void *callee, OP_EventType type, void *data)
{
	VRayExporter *exporter = (VRayExporter*)callee;

	PRINT_INFO("RtCallbackVop: %s from \"%s\"",
			   OPeventToString(type), caller->getName().buffer());

	if (type == OP_PARM_CHANGED ||
		type == OP_INPUT_CHANGED)
	{
		exporter->exportVop(caller);
	}
	else if (type == OP_NODE_PREDELETE) {
		exporter->delOpCallback(caller, VRayExporter::RtCallbackVop);
	}
}


VRay::Plugin VRayExporter::exportVop(OP_Node *op_node)
{
	VOP_Node *vop_node = op_node->castToVOPNode();

	const UT_String &opType = vop_node->getOperator()->getName();

	PRINT_INFO("Exporting node \"%s\" [%s]...",
			   vop_node->getName().buffer(),
			   opType.buffer())

			if (opType == "switch") {
		const int &switcher = vop_node->evalInt("switcher", 0, 0.0);
		if (switcher > 0) {
			UT_String inputName;
			inputName.sprintf("input%i", switcher);

			return exportConnectedVop(vop_node, inputName);
		}
	}
	else if (opType.startsWith("VRayNode")) {
		VOP::NodeBase *vrayNode = static_cast<VOP::NodeBase*>(vop_node);

		addOpCallback(op_node, VRayExporter::RtCallbackVop);

		Attrs::PluginDesc pluginDesc;

		OP::VRayNode::PluginResult res = vrayNode->asPluginDesc(pluginDesc, this, op_node);
		if (res == OP::VRayNode::PluginResultError) {
			PRINT_ERROR("Error creating plugin descripion for node: \"%s\" [%s]",
						vop_node->getName().buffer(),
						opType.buffer());
		}
		else if (res == OP::VRayNode::PluginResultNA ||
				 res == OP::VRayNode::PluginResultContinue)
		{
			pluginDesc.pluginName = VRayExporter::getPluginName(vop_node);
			pluginDesc.pluginID   = vrayNode->getVRayPluginID();

			setAttrsFromOpNode(pluginDesc, op_node);

			if (vrayNode->getVRayPluginType() == "RENDERCHANNEL") {
				Attrs::PluginAttr *attr_chan_name = pluginDesc.get("name");
				if (NOT(attr_chan_name) || attr_chan_name->paramValue.valString.empty()) {
					const std::string channelName = op_node->getName().buffer();
					if (NOT(attr_chan_name)) {
						pluginDesc.addAttribute(Attrs::PluginAttr("name", channelName));
					}
					else {
						attr_chan_name->paramValue.valString = channelName;
					}
				}
			}

			if (pluginDesc.pluginID == "PhxShaderSimVol") {
				// "phoenix_sim" attribute is a List()
				//
				Attrs::PluginAttr *attr_phoenix_sim = pluginDesc.get("phoenix_sim");
				if (attr_phoenix_sim) {
					attr_phoenix_sim->paramType = Attrs::PluginAttr::AttrTypeListValue;
					attr_phoenix_sim->paramValue.valListValue.push_back(VRay::Value(attr_phoenix_sim->paramValue.valPlugin));
				}
			}

			return exportPlugin(pluginDesc);
		}
	}
	else {
		PRINT_ERROR("Unsupported VOP node: %s",
					opType.buffer());
	}

	return VRay::Plugin();
}


VRay::Plugin VRayExporter::exportMaterial(SHOP_Node *shop_node)
{
	VRay::Plugin material;

	OP_Node *op_node = VRayExporter::FindChildNodeByType(shop_node, "vray_material_output");
	if (!op_node) {
		PRINT_ERROR("Can't find \"V-Ray Material Output\" operator under \"%s\"!",
					shop_node->getName().buffer());
	}
	else {
		VOP::MaterialOutput *mtl_out = static_cast<VOP::MaterialOutput *>(op_node);
		addOpCallback(mtl_out, VRayExporter::RtCallbackShop);

		if (mtl_out->error() < UT_ERROR_ABORT ) {
			PRINT_INFO("Exporting material output \"%s\"...",
					   mtl_out->getName().buffer());

			const int idx = mtl_out->getInputFromName("Material");
			VOP::NodeBase *input = dynamic_cast<VOP::NodeBase*>(mtl_out->getInput(idx));
			if (input) {
				switch (mtl_out->getInputType(idx)) {
					case VOP_SURFACE_SHADER: {
						material = exportVop(input);
						break;
					}
					case VOP_TYPE_BSDF: {
						VRay::Plugin pluginBRDF = exportVop(input);

						// Wrap BRDF into MtlSingleBRDF for RT GPU to work properly
						Attrs::PluginDesc mtlPluginDesc(input, "MtlSingleBRDF", "Mtl@");
						mtlPluginDesc.addAttribute(Attrs::PluginAttr("brdf", pluginBRDF));

						material = exportPlugin(mtlPluginDesc);
						break;
					}
					default:
						PRINT_ERROR("Unsupported input type for node \"%s\", input %d!",
									mtl_out->getName().buffer(), idx);
				}

				if (material) {
					// Wrap material into MtlRenderStats to always have the same material name
					// Used when rewiring materials when running interactive RT session
					// TODO: Do not use for non-interactive export
					Attrs::PluginDesc pluginDesc(mtl_out->getParent(), "MtlRenderStats", "Mtl@");
					pluginDesc.addAttribute(Attrs::PluginAttr("base_mtl", material));
					material = exportPlugin(pluginDesc);
				}
			}
		}
	}

	return material;
}


void VRayExporter::RtCallbackShop(OP_Node *caller, void *callee, OP_EventType type, void *data)
{
	VRayExporter *exporter = (VRayExporter*)callee;

	PRINT_INFO("RtCallbackDisplacement: %s from \"%s\"",
			   OPeventToString(type), caller->getName().buffer());

	if (type == OP_PARM_CHANGED ||
		type == OP_INPUT_REWIRED)
	{
		UT_String callerPath;
		caller->getFullPath(callerPath);

		UT_String shopPath;
		caller->getParent()->getFullPath(shopPath);
		SHOP_Node *shop_node = OPgetDirector()->findSHOPNode(shopPath.buffer());

		OP_NodeList refs;
		shop_node->getExistingOpDependents(refs, true);
		for (OP_Node *node : refs) {
			UT_String nodePath;
			node->getFullPath(nodePath);

			OBJ_Node *obj_node = node->castToOBJNode();
			if (obj_node) {
				exporter->exportObject(obj_node);
				continue;
			}
			SOP_Node *sop_node = node->castToSOPNode();
			if (sop_node) {
				obj_node = sop_node->getParent()->castToOBJNode();
				exporter->exportObject(obj_node);
				continue;
			}
		}
	}
	else if (type == OP_NODE_PREDELETE) {
		exporter->delOpCallback(caller, VRayExporter::RtCallbackShop);
	}
}


VRay::Plugin VRayExporter::exportDisplacement(OBJ_Node *obj_node, VRay::Plugin &geomPlugin)
{
	VRay::Plugin plugin;

	UT_String shopPath;
	obj_node->evalString(shopPath, "vray_displacement", 0, 0.0f);
	SHOP_Node *shop_node = OPgetDirector()->findSHOPNode(shopPath.buffer());
	if (NOT(shop_node)) {
		// Take displacement from the shop_materialpath
		shop_node = obj_node->getMaterialNode(m_context.getTime());
		if (NOT(shop_node)) {
			return plugin;
		}
	}

	OP_Node *op_node = VRayExporter::FindChildNodeByType(shop_node, "vray_material_output");
	if (!op_node) {
		PRINT_ERROR("Can't find \"V-Ray Material Output\" operator under \"%s\"!",
					shop_node->getName().buffer());
	}
	else {
		VOP::MaterialOutput *mtl_out = static_cast<VOP::MaterialOutput *>(op_node);

		addOpCallback(op_node, VRayExporter::RtCallbackShop);

		const int idx = mtl_out->getInputFromName("Geometry");
		VOP::NodeBase *input = dynamic_cast<VOP::NodeBase*>(mtl_out->getInput(idx));
		if (input && mtl_out->getInputType(idx) == VOP_GEOMETRY_SHADER) {
			VOP::NodeBase *displ = static_cast<VOP::NodeBase *>(input);
			Attrs::PluginDesc pluginDesc(VRayExporter::getPluginName(obj_node, "Geom@"), displ->getVRayPluginID());
			pluginDesc.addAttribute(Attrs::PluginAttr("mesh", geomPlugin));

			OP::VRayNode::PluginResult res = displ->asPluginDesc(pluginDesc, this, obj_node);
			if (res == OP::VRayNode::PluginResultError) {
				PRINT_ERROR("Error creating plugin descripion for node: \"%s\" [%s]",
							displ->getName().buffer(), displ->getOperator()->getName().buffer());
			}
			else if (res == OP::VRayNode::PluginResultNA ||
					 res == OP::VRayNode::PluginResultContinue)
			{
				setAttrsFromOpNode(pluginDesc, displ);
			}

			plugin = exportPlugin(pluginDesc);
		}
	}

	return plugin;
}


VRay::Plugin VRayExporter::exportDefaultMaterial()
{
	Attrs::PluginDesc brdfDesc("BRDFDiffuse@Clay", "BRDFDiffuse");
	brdfDesc.addAttribute(Attrs::PluginAttr("color", 0.5f, 0.5f, 0.5f));

	Attrs::PluginDesc mtlDesc("Mtl@Clay", "MtlSingleBRDF");
	mtlDesc.addAttribute(Attrs::PluginAttr("brdf", exportPlugin(brdfDesc)));

	return exportPlugin(mtlDesc);
}


#ifdef CGR_HAS_VRAYSCENE
VRay::Plugin VRayExporter::exportVRayScene(OBJ_Node *obj_node, SOP_Node *geom_node)
{
	SOP::VRayScene *vraySceneNode = static_cast<SOP::VRayScene*>(geom_node);

	Attrs::PluginDesc pluginDesc;
	OP::VRayNode::PluginResult res = vraySceneNode->asPluginDesc(pluginDesc, this, static_cast<OP_Node*>(obj_node));
	if (res == OP::VRayNode::PluginResultSuccess) {
		const bool flip_axis = vraySceneNode->evalInt("flip_axis", 0, 0.0f);

		pluginDesc.addAttribute(Attrs::PluginAttr("transform", VRayExporter::GetOBJTransform(obj_node, m_context, flip_axis)));

		return exportPlugin(pluginDesc);
	}

	return VRay::Plugin();
}

#endif // CGR_HAS_VRAYSCENE


static std::string ObjectTypeToString(const OBJ_OBJECT_TYPE &ob_type)
{
	std::string object_type;

	if (ob_type & OBJ_WORLD) {
		object_type += " | OBJ_WORLD";
	}
	if (ob_type & OBJ_GEOMETRY) {
		object_type += " | OBJ_GEOMETRY";
	}
	if (ob_type & OBJ_CAMERA) {
		object_type += " | OBJ_CAMERA";
	}
	if (ob_type & OBJ_LIGHT) {
		object_type += " | OBJ_LIGHT";
	}
	if (ob_type & OBJ_RENDERER) {
		object_type += " | OBJ_RENDERER";
	}
	if (ob_type & OBJ_FOG) {
		object_type += " | OBJ_FOG";
	}
	if (ob_type & OBJ_BONE) {
		object_type += " | OBJ_BONE";
	}
	if (ob_type & OBJ_HANDLE) {
		object_type += " | OBJ_HANDLE";
	}
	if (ob_type & OBJ_BLEND) {
		object_type += " | OBJ_BLEND";
	}
	if (ob_type & OBJ_FORCE) {
		object_type += " | OBJ_FORCE";
	}
	if (ob_type & OBJ_CAMSWITCH) {
		object_type += " | OBJ_CAMSWITCH";
	}
	if (ob_type & OBJ_SOUND) {
		object_type += " | OBJ_SOUND";
	}
	if (ob_type & OBJ_MICROPHONE) {
		object_type += " | OBJ_MICROPHONE";
	}
	if (ob_type & OBJ_SUBNET) {
		object_type += " | OBJ_SUBNET";
	}
	if (ob_type & OBJ_FETCH) {
		object_type += " | OBJ_FETCH";
	}
	if (ob_type & OBJ_NULL) {
		object_type += " | OBJ_NULL";
	}
	if (ob_type & OBJ_STICKY) {
		object_type += " | OBJ_STICKY";
	}
	if (ob_type & OBJ_DOPNET) {
		object_type += " | OBJ_DOPNET";
	}
	if (ob_type & OBJ_RIVET) {
		object_type += " | OBJ_RIVET";
	}
	if (ob_type & OBJ_MUSCLE) {
		object_type += " | OBJ_MUSCLE";
	}

	return object_type;
}


void VRayExporter::TraverseOBJ(OBJ_Node *obj_node, void *data)
{
	VRayExporter *exporter = (VRayExporter*)data;
	const fpreal &t = exporter->getContext().getTime();

	if (obj_node) {
		const OBJ_OBJECT_TYPE &ob_type = obj_node->getObjectType();

		PRINT_INFO("Processing %s node: \"%s\"%s [%i|%i]",
				   obj_node->getOpType(),
				   obj_node->getName().buffer(),
				   ObjectTypeToString(ob_type).c_str(),
				   obj_node->getVisible(),
				   obj_node->isObjectRenderable(t));
	}

	if (obj_node && obj_node->getVisible()) {
		const OBJ_OBJECT_TYPE &ob_type = obj_node->getObjectType();

		PRINT_INFO("Processing node %s:\"%s\"%s [%i]",
				   obj_node->getOpType(),
				   obj_node->getName().buffer(),
				   ObjectTypeToString(ob_type).c_str(),
				   obj_node->isObjectRenderable(t));

		if (ob_type & OBJ_NULL) {
			return;
		}
		else if (ob_type & OBJ_LIGHT) {
			exporter->exportLight(obj_node);
		}
		else if (ob_type & OBJ_CAMERA) {
			/* Must go after OBJ_LIGHT */
			return;
		}
		else if (ob_type == OBJ_GEOMETRY) {
			exporter->exportObject(obj_node);
		}
#if 0
		else if (ob_type & OBJ_DOPNET) {
			exporter->exportParticles(obj_node);
		}
#endif
		else if (ob_type & OBJ_SUBNET) {
			// NOTE: This will handle hair
			OBJ_SubNet *obj_subnet = obj_node->castToOBJSubNet();
			if (obj_subnet) {
				OP_Bundle *op_bundle = obj_subnet->getVisibleObjectBundle(t);
				if (op_bundle) {
					const int op_count = op_bundle->entries();
					for (int i = 0; i < op_count; ++i) {
						OBJ_Node *b_obj_node = op_bundle->getNode(i)->castToOBJNode();
						if (b_obj_node) {
							TraverseOBJ(b_obj_node->castToOBJNode(), data);
						}
					}
				}
			}
		}
		else {
			OP_Node *op_node = obj_node->getRenderNodePtr();
			if (op_node) {
				PRINT_INFO("Found render node: %s",
						   op_node->getName().buffer());
				TraverseOBJ(op_node->castToOBJNode(), data);
			}
		}
	}
}


int VRayExporter::isRt()
{
	int is_rt = false;
	if (m_renderMode >= VRay::RendererOptions::RENDER_MODE_RT_CPU) {
		is_rt = true;
	}
	return is_rt;
}


int VRayExporter::isRtRunning()
{
	return m_renderer.isRtRunning();
}


int VRayExporter::isAborted()
{
	return m_is_aborted;
}


void VRayExporter::resetOpCallbacks()
{
	for (auto const &item : m_opRegCallbacks) {
		if (item.op_node->hasOpInterest(this, item.cb)) {
			item.op_node->removeOpInterest(this, item.cb);
		}
	}

	m_opRegCallbacks.clear();
}


void VRayExporter::addOpCallback(OP_Node *op_node, OP_EventMethod cb)
{
	if (isRt() && !op_node->hasOpInterest(this, cb)) {
		PRINT_INFO("addOpInterest(%s)",
				   op_node->getName().buffer());

		op_node->addOpInterest(this, cb);

		// Store registered callback for faster removal
		m_opRegCallbacks.push_back(CbItem(op_node, cb, this));
	}
}


void VRayExporter::delOpCallback(OP_Node *op_node, OP_EventMethod cb)
{
	if (op_node->hasOpInterest(this, cb)) {
		PRINT_INFO("removeOpInterest(%s)",
				   op_node->getName().buffer());

		op_node->removeOpInterest(this, cb);
	}
}


void VRayExporter::delOpCallbacks(OP_Node *op_node)
{
	m_opRegCallbacks.erase(std::remove_if(m_opRegCallbacks.begin(), m_opRegCallbacks.end(),
										  [op_node](CbItem &item) { return item.op_node == op_node; }), m_opRegCallbacks.end());
}


void VRayExporter::addRtCallbacks()
{
	m_renderer.addCbOnImageReady(CbVoid(boost::bind(&VRayExporter::resetOpCallbacks, this)));
	m_renderer.addCbOnRendererClose(CbVoid(boost::bind(&VRayExporter::resetOpCallbacks, this)));
}


void VRayExporter::removeRtCallbacks()
{
	resetOpCallbacks();
}


bool VRayExporter::TraverseOBJs(OP_Node &op_node, void *data)
{
	VRayExporter::TraverseOBJ(op_node.castToOBJNode(), data);
	return 0;
}


void VRayExporter::RtCallbackObjManager(OP_Node *caller, void *callee, OP_EventType type, void *data)
{
	VRayExporter *exporter = (VRayExporter*)callee;

	PRINT_INFO("RtCallbackObjManager: %s from \"%s\"",
			   OPeventToString(type), caller->getName().buffer());

	// NOTE: Use OP_GROUPLIST_CHANGED instead of OP_CHILD_CREATED,
	// OP_CHILD_CREATED gives intermediate node, not the final one
	if (type == OP_GROUPLIST_CHANGED ||
		type == OP_CHILD_REORDERED || /* undo */
		type == OP_CHILD_DELETED)
	{
		OP_Network *obj_manager = OPgetDirector()->getManager("obj");
		obj_manager->traverseChildren(VRayExporter::TraverseOBJs, exporter, false);
	}
	else if (type == OP_CHILD_CREATED) {
		VRayExporter::TraverseOBJ(reinterpret_cast<OBJ_Node*>(data), exporter);
	}
	else if (type == OP_NODE_PREDELETE) {
		exporter->delOpCallback(reinterpret_cast<OBJ_Node*>(data), VRayExporter::RtCallbackObjManager);
	}
}


int VRayExporter::exportScene()
{
	OP_Network *obj_manager = OPgetDirector()->getManager("obj");

	// NOTE: Do not go recursively here, process childs manually
	obj_manager->traverseChildren(VRayExporter::TraverseOBJs, this, false);

	addOpCallback(obj_manager, VRayExporter::RtCallbackObjManager);

	// Add simulations from OBJ
	if (m_phxSimulations.size()) {
		Attrs::PluginDesc phxSims("VRayNodePhxShaderSimVol", "PhxShaderSimVol");
		phxSims.addAttribute(Attrs::PluginAttr("phoenix_sim", m_phxSimulations));

		exportPlugin(phxSims);
	}

	return 0;
}


void VRayExporter::exportDone()
{
	m_renderer.syncObjects();
}


VRay::Plugin VRayExporter::exportPlugin(const Attrs::PluginDesc &pluginDesc)
{
	return m_renderer.exportPlugin(pluginDesc);
}


void VRayExporter::removePlugin(OBJ_Node *node)
{
	removePlugin(Attrs::PluginDesc(VRayExporter::getPluginName(node), ""));
}


void VRayExporter::removePlugin(const std::string &pluginName)
{
	removePlugin(Attrs::PluginDesc(pluginName, ""));
}


void VRayExporter::removePlugin(const Attrs::PluginDesc &pluginDesc)
{
	m_renderer.removePlugin(pluginDesc);
}


void VRayExporter::setFrame(float frame)
{
	m_renderer.setFrame(frame);
}


void VRayExporter::setRenderSize(int w, int h)
{
	m_renderer.setImageSize(w, h);
}


int VRayExporter::renderFrame(int locked)
{
	if (m_workMode == ExpWorkMode::ExpRender || m_workMode == ExpWorkMode::ExpExportRender) {
		m_renderer.startRender(locked);
	}
	if (m_workMode == ExpWorkMode::ExpExport || m_workMode == ExpWorkMode::ExpExportRender) {
		if (m_exportFilepath.empty()) {
		}
		else {
			exportVrscene(m_exportFilepath);
		}
	}
	return 0;
}


int VRayExporter::renderSequence(int start, int end, int step, int locked)
{
	return m_renderer.startSequence(start, end, step, locked);
}


int VRayExporter::exportVrscene(const std::string &filepath)
{
	return m_renderer.exportScene(filepath);
}


int VRayExporter::clearKeyFrames(fpreal toTime)
{
	return m_renderer.clearFrames(toTime);
}


void VRayExporter::setAnimation(bool on)
{
	m_is_animation = on;

	m_renderer.setAnimation(on);
}


void VRayExporter::setAbortCb(VRay::VRayRenderer &renderer)
{
	if (renderer.isAborted()) {
		setAbort();
	}
}


void VRayExporter::addAbortCallback()
{
	m_renderer.addCbOnImageReady(CbOnImageReady(boost::bind(&VRayExporter::setAbortCb, this, _1)));
}
