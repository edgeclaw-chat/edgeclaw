#pragma once

// Bridge from the C vim-colorscheme parser to style::Palette / style::IPalette.
//
// A parsed theme is a sparse set of semantic roles. This module fills the gaps
// from the built-in dark palette, repairs the few invariants the built-in
// palettes are carefully designed to hold (surfaces must be distinguishable),
// and installs the result as the process-wide palette provider.

#include "edc_colorscheme.h"
#include "edc_style.hpp"

#include <memory>
#include <string>

namespace edc::theme
{

// A fixed Palette behind style::IPalette, usable directly as a provider.
class FixedSchemePalette : public style::IPalette
{
  public:
    explicit FixedSchemePalette(style::Palette p) : palette_(std::move(p)) {}
    const style::Palette &colors() const override
    {
        return palette_;
    }

  private:
    style::Palette palette_;
};

// Build a Palette from a sparse parse: every set role is used, every unset role
// falls back to |base| (the built-in dark palette by default). `base_light`
// chooses the light built-in as the fallback, which matters for light themes
// that only set a handful of groups.
style::Palette PaletteFromScheme(const edc_colorscheme &cs, const style::Palette &base, bool base_light);

// Load <edc_home>/colors/<name>.vim and make it the active provider. Returns
// false (leaving the current provider untouched) when the name is empty/"off"
// or the file cannot be parsed. On success the returned provider must be kept
// alive by the caller (it is also installed into style::palette_provider_slot).
std::shared_ptr<FixedSchemePalette> InstallScheme(const std::string &edc_home, const std::string &name);

// Remove any custom provider, restoring the built-in light/dark selection.
void ClearScheme();

} // namespace edc::theme
