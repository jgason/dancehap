// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// matte_engine.cpp — MatteEngine implementation.
//
// Phase 2.1: stub mode — all operations return false/empty.
// Phase 2.2: real ONNX Runtime implementation (#ifdef DANCEHAP_HAVE_ONNXRUNTIME).

#include "matte_engine.hpp"

// ===========================================================================
//  Real ONNX Runtime implementation (Phase 2.2)
// ===========================================================================
#ifdef DANCEHAP_HAVE_ONNXRUNTIME

// Phase 2.2: real ONNX Runtime implementation.
// Will use OrtCreateEnv, OrtCreateSession, OrtRun, etc.
// For now, stub-only.

namespace dancehap {

struct MatteEngine::Impl {
    bool ready = false;
};

MatteEngine::MatteEngine()
    : pimpl_(std::make_unique<Impl>())
{
}

MatteEngine::~MatteEngine() = default;

MatteEngine::MatteEngine(MatteEngine &&) noexcept = default;
MatteEngine &MatteEngine::operator=(MatteEngine &&) noexcept = default;

bool MatteEngine::loadModel(const std::string & /*model_path*/)
{
    // Phase 2.2: real ONNX Runtime model loading.
    return false;
}

bool MatteEngine::isReady() const
{
    return pimpl_->ready;
}

MatteMask MatteEngine::infer(const ImageFrame & /*input*/)
{
    // Phase 2.2: real ONNX Runtime inference.
    return MatteMask{};
}

} // namespace dancehap

// ===========================================================================
//  Stub implementation (no ONNX Runtime)
// ===========================================================================
#else // !DANCEHAP_HAVE_ONNXRUNTIME

namespace dancehap {

struct MatteEngine::Impl {
    bool ready = false;
};

MatteEngine::MatteEngine()
    : pimpl_(std::make_unique<Impl>())
{
}

MatteEngine::~MatteEngine() = default;

MatteEngine::MatteEngine(MatteEngine &&) noexcept = default;
MatteEngine &MatteEngine::operator=(MatteEngine &&) noexcept = default;

bool MatteEngine::loadModel(const std::string & /*model_path*/)
{
    // Stub: no ONNX Runtime available.
    return false;
}

bool MatteEngine::isReady() const
{
    return pimpl_->ready;
}

MatteMask MatteEngine::infer(const ImageFrame & /*input*/)
{
    // Stub: returns empty mask.
    return MatteMask{};
}

} // namespace dancehap

#endif // DANCEHAP_HAVE_ONNXRUNTIME