// A noise image that tiles: what a blend material's or a road's edges are broken up by.
#pragma once

#include <godot_cpp/classes/fast_noise_lite.hpp>
#include <godot_cpp/classes/noise_texture2d.hpp>

namespace hewn {

inline godot::Ref<godot::NoiseTexture2D> make_noise_texture() {
	godot::Ref<godot::FastNoiseLite> n;
	n.instantiate();
	n->set_frequency(0.02f);
	n->set_fractal_octaves(4);
	godot::Ref<godot::NoiseTexture2D> t;
	t.instantiate();
	t->set_width(256);
	t->set_height(256);
	t->set_seamless(true);
	t->set_generate_mipmaps(true);
	t->set_noise(n);
	return t;
}

} // namespace hewn
