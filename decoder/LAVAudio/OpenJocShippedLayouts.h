/*
 * SPDX-FileCopyrightText: 2026 OpenJOC contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// pattern: Functional Core

#pragma once

#include "LAVOpenJocSettings.h"

#include <cstddef>

const LAVOpenJocOutputPolicy *GetLAVOpenJocShippedOutputPolicies(std::size_t *count) noexcept;
bool IsLAVOpenJocOutputPolicyShipped(LAVOpenJocOutputPolicy policy) noexcept;
