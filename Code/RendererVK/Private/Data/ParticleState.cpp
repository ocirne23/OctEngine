module RendererVK;

import Core;
import Settings;
import :ParticleState;

const ParticleParams& ParticleState::getParams() const { return Globals::settings.particles; }
bool ParticleState::isEnabled() const { return Globals::settings.particles.enabled; }
