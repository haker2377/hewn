// FBX export: see hw_fbx.h. The file is a tree of records (a name, typed values, records inside); offsets are 32 bit
// (version 7400). Y is up and a unit is a meter (UnitScaleFactor: centimeters per unit). A model keeps its place;
// its turn and scale are put into its vertices.
#include "hw_fbx.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include <cstring>
#include <map>
#include <vector>

using namespace godot;

namespace hewn {

namespace {

struct Prop {
	char type;
	std::vector<uint8_t> data;
};

struct Rec {
	String name;
	std::vector<Prop> props;
	std::vector<Rec> kids;

	Rec(const String &p_name = String()) :
			name(p_name) {}

	template <typename T>
	static void put(std::vector<uint8_t> &r, const T &p_v) {
		const uint8_t *b = reinterpret_cast<const uint8_t *>(&p_v);
		r.insert(r.end(), b, b + sizeof(T));
	}
	Rec &i32(int32_t p_v) {
		Prop p{ 'I', {} };
		put(p.data, p_v);
		props.push_back(p);
		return *this;
	}
	Rec &i64(int64_t p_v) {
		Prop p{ 'L', {} };
		put(p.data, p_v);
		props.push_back(p);
		return *this;
	}
	Rec &f64(double p_v) {
		Prop p{ 'D', {} };
		put(p.data, p_v);
		props.push_back(p);
		return *this;
	}
	Rec &bln(bool p_v) {
		Prop p{ 'C', {} };
		p.data.push_back(p_v ? 1 : 0);
		props.push_back(p);
		return *this;
	}
	Rec &raw(const std::vector<uint8_t> &p_bytes, char p_type = 'S') {
		Prop p{ p_type, {} };
		put(p.data, (uint32_t)p_bytes.size());
		p.data.insert(p.data.end(), p_bytes.begin(), p_bytes.end());
		props.push_back(p);
		return *this;
	}
	Rec &str(const String &p_s) {
		const CharString u = p_s.utf8();
		return raw(std::vector<uint8_t>(u.get_data(), u.get_data() + u.length()));
	}
	// an object's name: "name\x00\x01Class"
	Rec &obj_name(const String &p_name, const char *p_class) {
		const CharString u = p_name.utf8();
		std::vector<uint8_t> b(u.get_data(), u.get_data() + u.length());
		b.push_back(0);
		b.push_back(1);
		b.insert(b.end(), p_class, p_class + std::strlen(p_class));
		return raw(b);
	}
	template <typename T>
	Rec &arr(char p_type, const std::vector<T> &p_v) {
		Prop p{ p_type, {} };
		put(p.data, (uint32_t)p_v.size());
		put(p.data, (uint32_t)0); // (not compressed)
		put(p.data, (uint32_t)(p_v.size() * sizeof(T)));
		const uint8_t *b = reinterpret_cast<const uint8_t *>(p_v.data());
		p.data.insert(p.data.end(), b, b + p_v.size() * sizeof(T));
		props.push_back(p);
		return *this;
	}
	Rec &add(const Rec &p_kid) {
		kids.push_back(p_kid);
		return kids.back();
	}
	Rec &child(const String &p_name) {
		kids.push_back(Rec(p_name));
		return kids.back();
	}
};

void write_rec(std::vector<uint8_t> &r_out, const Rec &p_rec) {
	const size_t start = r_out.size();
	Rec::put(r_out, (uint32_t)0); // (the end offset, filled below)
	Rec::put(r_out, (uint32_t)p_rec.props.size());
	uint32_t plen = 0;
	for (const Prop &p : p_rec.props) {
		plen += 1 + (uint32_t)p.data.size();
	}
	Rec::put(r_out, plen);
	const CharString n = p_rec.name.utf8();
	r_out.push_back((uint8_t)n.length());
	r_out.insert(r_out.end(), n.get_data(), n.get_data() + n.length());
	for (const Prop &p : p_rec.props) {
		r_out.push_back((uint8_t)p.type);
		r_out.insert(r_out.end(), p.data.begin(), p.data.end());
	}
	if (!p_rec.kids.empty() || p_rec.props.empty()) {
		for (const Rec &k : p_rec.kids) {
			write_rec(r_out, k);
		}
		r_out.insert(r_out.end(), 13, 0); // (the end of the records inside)
	}
	const uint32_t end = (uint32_t)r_out.size();
	std::memcpy(r_out.data() + start, &end, 4);
}

// P: a property of Properties70 (its name, type, label, flags, then its values)
Rec &prop70(Rec &p_props, const char *p_name, const char *p_type, const char *p_label, const char *p_flags) {
	Rec &p = p_props.child("P");
	p.str(p_name).str(p_type).str(p_label).str(p_flags);
	return p;
}

void collect(Node *p_node, const Transform3D &p_xf, std::vector<std::pair<MeshInstance3D *, Transform3D>> &r_out) {
	Transform3D xf = p_xf;
	if (Node3D *n3 = Object::cast_to<Node3D>(p_node)) {
		xf = p_xf * n3->get_transform();
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_node);
		if (mi && mi->get_mesh().is_valid() && mi->is_visible()) {
			r_out.push_back({ mi, xf });
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		collect(p_node->get_child(i), xf, r_out);
	}
}

} // namespace

Error write_fbx(Node *p_root, const String &p_path) {
	ERR_FAIL_NULL_V(p_root, ERR_INVALID_PARAMETER);
	std::vector<std::pair<MeshInstance3D *, Transform3D>> items;
	for (int i = 0; i < p_root->get_child_count(); i++) {
		collect(p_root->get_child(i), Transform3D(), items);
	}
	if (MeshInstance3D *self = Object::cast_to<MeshInstance3D>(p_root)) {
		items.insert(items.begin(), { self, Transform3D() });
	}
	int64_t next_id = 1000000;
	Rec objects("Objects");
	Rec conns("Connections");
	std::map<uint64_t, int64_t> mat_ids; // (a material once, shared by the models that use it)
	int n_models = 0, n_geoms = 0, n_mats = 0, n_tex = 0;
	for (const auto &it : items) {
		MeshInstance3D *mi = it.first;
		const Transform3D &xf = it.second;
		Ref<Mesh> mesh = mi->get_mesh();
		std::vector<double> verts, normals, uvs;
		std::vector<int32_t> poly, uv_index, mat_of_poly;
		std::vector<Ref<Material>> mats;
		for (int s = 0; s < mesh->get_surface_count(); s++) {
			const Array a = mesh->surface_get_arrays(s);
			const PackedVector3Array v = a[Mesh::ARRAY_VERTEX];
			if (v.is_empty()) {
				continue;
			}
			const PackedVector3Array nrm = a[Mesh::ARRAY_NORMAL];
			const PackedVector2Array uv = a[Mesh::ARRAY_TEX_UV];
			PackedInt32Array idx = a[Mesh::ARRAY_INDEX];
			if (idx.is_empty()) {
				for (int k = 0; k < v.size(); k++) {
					idx.push_back(k);
				}
			}
			Ref<Material> m = mi->get_surface_override_material(s);
			if (m.is_null()) {
				m = mi->get_material_override().is_valid() ? mi->get_material_override() : mesh->surface_get_material(s);
			}
			int slot = -1;
			for (size_t k = 0; k < mats.size(); k++) {
				if (mats[k] == m) {
					slot = (int)k;
				}
			}
			if (slot < 0) {
				slot = (int)mats.size();
				mats.push_back(m);
			}
			const int base = (int)(verts.size() / 3);
			const Basis nb = xf.basis.inverse().transposed();
			for (int k = 0; k < v.size(); k++) {
				const Vector3 p = xf.basis.xform(v[k]);
				verts.push_back(p.x);
				verts.push_back(p.y);
				verts.push_back(p.z);
				const Vector2 t = k < uv.size() ? uv[k] : Vector2();
				uvs.push_back(t.x);
				uvs.push_back(1.0 - t.y); // (FBX: V up)
			}
			// (Godot's front faces are clockwise, FBX's counter-clockwise)
			for (int64_t k = 0; k + 2 < idx.size(); k += 3) {
				const int c[3] = { idx[k], idx[k + 2], idx[k + 1] };
				for (int q = 0; q < 3; q++) {
					poly.push_back(q == 2 ? -(base + c[q]) - 1 : base + c[q]);
					uv_index.push_back(base + c[q]);
					const Vector3 n = c[q] < nrm.size() ? nb.xform(nrm[c[q]]).normalized() : Vector3(0, 1, 0);
					normals.push_back(n.x);
					normals.push_back(n.y);
					normals.push_back(n.z);
				}
				mat_of_poly.push_back(slot);
			}
		}
		if (poly.empty()) {
			continue;
		}
		const int64_t geo_id = next_id++, model_id = next_id++;
		const String name = String(mi->get_name()).is_empty() ? String("Mesh") : String(mi->get_name());
		Rec &geo = objects.child("Geometry");
		geo.i64(geo_id).obj_name(name, "Geometry").str("Mesh");
		geo.child("Vertices").arr('d', verts);
		geo.child("PolygonVertexIndex").arr('i', poly);
		geo.child("GeometryVersion").i32(124);
		{
			Rec &e = geo.child("LayerElementNormal");
			e.i32(0);
			e.child("Version").i32(102);
			e.child("Name").str("");
			e.child("MappingInformationType").str("ByPolygonVertex");
			e.child("ReferenceInformationType").str("Direct");
			e.child("Normals").arr('d', normals);
		}
		{
			Rec &e = geo.child("LayerElementUV");
			e.i32(0);
			e.child("Version").i32(101);
			e.child("Name").str("UVMap");
			e.child("MappingInformationType").str("ByPolygonVertex");
			e.child("ReferenceInformationType").str("IndexToDirect");
			e.child("UV").arr('d', uvs);
			e.child("UVIndex").arr('i', uv_index);
		}
		{
			Rec &e = geo.child("LayerElementMaterial");
			e.i32(0);
			e.child("Version").i32(101);
			e.child("Name").str("");
			e.child("MappingInformationType").str("ByPolygon");
			e.child("ReferenceInformationType").str("IndexToDirect");
			e.child("Materials").arr('i', mat_of_poly);
		}
		{
			Rec &l = geo.child("Layer");
			l.i32(0);
			l.child("Version").i32(100);
			for (const char *t : { "LayerElementNormal", "LayerElementUV", "LayerElementMaterial" }) {
				Rec &le = l.child("LayerElement");
				le.child("Type").str(t);
				le.child("TypedIndex").i32(0);
			}
		}
		n_geoms++;
		Rec &model = objects.child("Model");
		model.i64(model_id).obj_name(name, "Model").str("Mesh");
		model.child("Version").i32(232);
		{
			Rec &p70 = model.child("Properties70");
			prop70(p70, "Lcl Translation", "Lcl Translation", "", "A").f64(xf.origin.x).f64(xf.origin.y).f64(xf.origin.z);
		}
		model.child("Shading").bln(true);
		model.child("Culling").str("CullingOff");
		n_models++;
		conns.child("C").str("OO").i64(model_id).i64(0);
		conns.child("C").str("OO").i64(geo_id).i64(model_id);
		for (const Ref<Material> &m : mats) {
			const uint64_t key = m.is_valid() ? m->get_instance_id() : 0;
			auto found = mat_ids.find(key);
			int64_t mat_id;
			if (found != mat_ids.end()) {
				mat_id = found->second;
			} else {
				mat_id = next_id++;
				mat_ids[key] = mat_id;
				String mname = m.is_valid() && !m->get_path().is_empty() ? m->get_path().get_file().get_basename() : (m.is_valid() ? String(m->get_name()) : String());
				if (mname.is_empty()) {
					mname = "Material" + itos(n_mats + 1);
				}
				Rec &mr = objects.child("Material");
				mr.i64(mat_id).obj_name(mname, "Material").str("");
				mr.child("Version").i32(102);
				mr.child("ShadingModel").str("phong");
				mr.child("MultiLayer").i32(0);
				const Ref<BaseMaterial3D> bm = m;
				const Color c = bm.is_valid() ? bm->get_albedo() : Color(0.8f, 0.8f, 0.8f);
				Rec &p70 = mr.child("Properties70");
				prop70(p70, "DiffuseColor", "Color", "", "A").f64(c.r).f64(c.g).f64(c.b);
				n_mats++;
				const Ref<Texture2D> tex = bm.is_valid() ? bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO) : Ref<Texture2D>();
				if (tex.is_valid() && !tex->get_path().is_empty() && !tex->get_path().contains("::")) {
					const String abs = ProjectSettings::get_singleton()->globalize_path(tex->get_path());
					const int64_t tex_id = next_id++, vid_id = next_id++;
					const String tname = tex->get_path().get_file().get_basename();
					Rec &vr = objects.child("Video");
					vr.i64(vid_id).obj_name(tname, "Video").str("Clip");
					vr.child("Type").str("Clip");
					vr.child("FileName").str(abs);
					vr.child("RelativeFilename").str(tex->get_path().get_file());
					Rec &tr = objects.child("Texture");
					tr.i64(tex_id).obj_name(tname, "Texture").str("");
					tr.child("Type").str("TextureVideoClip");
					tr.child("Version").i32(202);
					tr.child("TextureName").obj_name(tname, "Texture");
					tr.child("Media").obj_name(tname, "Video");
					tr.child("FileName").str(abs);
					tr.child("RelativeFilename").str(tex->get_path().get_file());
					conns.child("C").str("OO").i64(vid_id).i64(tex_id);
					conns.child("C").str("OP").i64(tex_id).i64(mat_id).str("DiffuseColor");
					n_tex++;
				}
			}
			conns.child("C").str("OO").i64(mat_id).i64(model_id);
		}
	}
	ERR_FAIL_COND_V_MSG(n_models == 0, ERR_INVALID_DATA, "Hewn: nothing to put in the FBX file.");

	static const uint8_t FILE_ID[16] = { 0x28, 0xb3, 0x2a, 0xeb, 0xb6, 0x24, 0xcc, 0xc2, 0xbf, 0xc8, 0xb0, 0x2a, 0xa9, 0x2b, 0xfc, 0xf1 };
	static const uint8_t FOOT_ID[16] = { 0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66, 0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e };
	static const uint8_t END_MAGIC[16] = { 0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e, 0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b };
	std::vector<Rec> top;
	{
		Rec h("FBXHeaderExtension");
		h.child("FBXHeaderVersion").i32(1003);
		h.child("FBXVersion").i32(7400);
		h.child("Creator").str("Hewn");
		top.push_back(h);
		Rec fid("FileId");
		fid.raw(std::vector<uint8_t>(FILE_ID, FILE_ID + 16), 'R');
		top.push_back(fid);
		top.push_back(Rec("CreationTime"));
		top.back().str("1970-01-01 10:00:00:000");
		top.push_back(Rec("Creator"));
		top.back().str("Hewn");
		Rec gs("GlobalSettings");
		gs.child("Version").i32(1000);
		Rec &p70 = gs.child("Properties70");
		prop70(p70, "UpAxis", "int", "Integer", "").i32(1);
		prop70(p70, "UpAxisSign", "int", "Integer", "").i32(1);
		prop70(p70, "FrontAxis", "int", "Integer", "").i32(2);
		prop70(p70, "FrontAxisSign", "int", "Integer", "").i32(1);
		prop70(p70, "CoordAxis", "int", "Integer", "").i32(0);
		prop70(p70, "CoordAxisSign", "int", "Integer", "").i32(1);
		prop70(p70, "OriginalUpAxis", "int", "Integer", "").i32(1);
		prop70(p70, "OriginalUpAxisSign", "int", "Integer", "").i32(1);
		prop70(p70, "UnitScaleFactor", "double", "Number", "").f64(100.0);
		prop70(p70, "OriginalUnitScaleFactor", "double", "Number", "").f64(100.0);
		top.push_back(gs);
		Rec defs("Definitions");
		defs.child("Version").i32(100);
		defs.child("Count").i32(n_models + n_geoms + n_mats + n_tex * 2 + 1);
		auto deftype = [&](const char *p_type, int p_count) {
			if (p_count > 0) {
				Rec &d = defs.child("ObjectType");
				d.str(p_type);
				d.child("Count").i32(p_count);
			}
		};
		deftype("GlobalSettings", 1);
		deftype("Model", n_models);
		deftype("Geometry", n_geoms);
		deftype("Material", n_mats);
		deftype("Texture", n_tex);
		deftype("Video", n_tex);
		top.push_back(defs);
		top.push_back(objects);
		top.push_back(conns);
	}
	std::vector<uint8_t> out;
	static const char MAGIC[] = "Kaydara FBX Binary  ";
	out.insert(out.end(), MAGIC, MAGIC + 20);
	out.push_back(0x00);
	out.push_back(0x1a);
	out.push_back(0x00);
	Rec::put(out, (uint32_t)7400);
	for (const Rec &r : top) {
		write_rec(out, r);
	}
	out.insert(out.end(), 13, 0); // (the end of the top records)
	out.insert(out.end(), FOOT_ID, FOOT_ID + 16);
	out.insert(out.end(), 4, 0);
	size_t pad = ((out.size() + 15) & ~(size_t)15) - out.size();
	if (pad == 0) {
		pad = 16;
	}
	out.insert(out.end(), pad, 0);
	Rec::put(out, (uint32_t)7400);
	out.insert(out.end(), 120, 0);
	out.insert(out.end(), END_MAGIC, END_MAGIC + 16);

	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(f.is_null(), ERR_CANT_CREATE, "Hewn: cannot write " + p_path);
	PackedByteArray bytes;
	bytes.resize((int64_t)out.size());
	std::memcpy(bytes.ptrw(), out.data(), out.size());
	f->store_buffer(bytes);
	return OK;
}

} // namespace hewn
