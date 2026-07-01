// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// tests/unit/test_matte_engine.cpp
//
// Phase 2.1/2.2: MatteEngine stub behavior + preprocess/postprocess helpers.

#include "matte_engine.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace dancehap {
namespace {

// ---------------------------------------------------------------------------
// Stub behavior (Phase 2.1)
// ---------------------------------------------------------------------------

TEST(MatteEngineTest, CanInstantiate)
{
    MatteEngine engine;
    EXPECT_FALSE(engine.isReady());
}

TEST(MatteEngineTest, LoadModelReturnsFalseInStub)
{
    MatteEngine engine;
    EXPECT_FALSE(engine.loadModel("any_model.onnx"));
    EXPECT_FALSE(engine.isReady());
}

TEST(MatteEngineTest, InferReturnsEmptyInStub)
{
    MatteEngine engine;
    ImageFrame input{256, 256, nullptr};
    MatteMask mask = engine.infer(input);
    EXPECT_EQ(mask.width, 0);
    EXPECT_EQ(mask.height, 0);
    EXPECT_TRUE(mask.alpha.empty());
}

// ---------------------------------------------------------------------------
// Preprocess helper (Phase 2.2)
// ---------------------------------------------------------------------------

TEST(MatteEngineTest, PreprocessConvertsRgbaToRgbNormalized)
{
    // 1x1 white RGBA pixel → R=G=B=1.0, normalized = (1.0 - mean) / std.
    uint8_t white[4] = {255, 255, 255, 255};
    ImageFrame input{1, 1, white};

    auto out = preprocess_rgba_to_rgb_normalized(input, 1, 1);
    ASSERT_EQ(out.size(), static_cast<size_t>(3));

    // CHW layout: R plane, G plane, B plane (1 sample each).
    float expected_r = (1.0f - 0.485f) / 0.229f;
    float expected_g = (1.0f - 0.456f) / 0.224f;
    float expected_b = (1.0f - 0.406f) / 0.225f;

    EXPECT_NEAR(out[0], expected_r, 0.01f);
    EXPECT_NEAR(out[1], expected_g, 0.01f);
    EXPECT_NEAR(out[2], expected_b, 0.01f);
}

TEST(MatteEngineTest, PreprocessHandlesZeroPixel)
{
    // 1x1 black RGBA pixel → R=G=B=0, normalized = (0 - mean) / std.
    uint8_t black[4] = {0, 0, 0, 255};
    ImageFrame input{1, 1, black};

    auto out = preprocess_rgba_to_rgb_normalized(input, 1, 1);
    ASSERT_EQ(out.size(), static_cast<size_t>(3));

    EXPECT_NEAR(out[0], -0.485f / 0.229f, 0.01f);
    EXPECT_NEAR(out[1], -0.456f / 0.224f, 0.01f);
    EXPECT_NEAR(out[2], -0.406f / 0.225f, 0.01f);
}

TEST(MatteEngineTest, PreprocessResizesToTargetDims)
{
    // 4x4 red RGBA → resize to 2x2. Output should have 3*2*2 = 12 elements.
    std::vector<uint8_t> red(4 * 4 * 4, 0);
    for (size_t i = 0; i < 16; ++i) {
        red[i * 4 + 0] = 255;  // R
    }
    ImageFrame input{4, 4, red.data()};

    auto out = preprocess_rgba_to_rgb_normalized(input, 2, 2);
    EXPECT_EQ(out.size(), static_cast<size_t>(3 * 2 * 2));
}

// ---------------------------------------------------------------------------
// Postprocess helper (Phase 2.2)
// ---------------------------------------------------------------------------

TEST(MatteEngineTest, PostprocessPhaToMask)
{
    // 1x1 pha = 0.75 → mask alpha = 0.75.
    float pha[1] = {0.75f};
    MatteMask mask = postprocess_pha_to_mask(pha, 1, 1, 1, 1);

    EXPECT_EQ(mask.width, 1);
    EXPECT_EQ(mask.height, 1);
    ASSERT_EQ(mask.alpha.size(), static_cast<size_t>(1));
    EXPECT_NEAR(mask.alpha[0], 0.75f, 0.01f);
}

TEST(MatteEngineTest, PostprocessClampsToValidRange)
{
    // Values outside [0,1] should be clamped.
    float pha[2] = {-0.5f, 1.5f};
    MatteMask mask = postprocess_pha_to_mask(pha, 2, 1, 2, 1);

    ASSERT_EQ(mask.alpha.size(), static_cast<size_t>(2));
    EXPECT_NEAR(mask.alpha[0], 0.0f, 0.001f);
    EXPECT_NEAR(mask.alpha[1], 1.0f, 0.001f);
}

TEST(MatteEngineTest, PostprocessResizesMask)
{
    // 1x1 pha = 0.5 → resize to 4x4 mask, all values should be ~0.5.
    float pha[1] = {0.5f};
    MatteMask mask = postprocess_pha_to_mask(pha, 1, 1, 4, 4);

    EXPECT_EQ(mask.width, 4);
    EXPECT_EQ(mask.height, 4);
    ASSERT_EQ(mask.alpha.size(), static_cast<size_t>(16));
    for (float v : mask.alpha) {
        EXPECT_NEAR(v, 0.5f, 0.01f);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Provider resolution (Phase 2.3)
// ---------------------------------------------------------------------------

TEST(MatteEngineTest, ResolveProviderAutoReturnsPlatformDefault)
{
    ActiveProvider ap = resolve_provider(ExecutionProvider::Auto);
#if defined(_WIN32)
    EXPECT_EQ(ap, ActiveProvider::DirectML);
#elif defined(__APPLE__)
    EXPECT_EQ(ap, ActiveProvider::CoreML);
#else
    EXPECT_EQ(ap, ActiveProvider::CPU);
#endif
}

TEST(MatteEngineTest, ResolveProviderCPUAlwaysCPU)
{
    EXPECT_EQ(resolve_provider(ExecutionProvider::CPU), ActiveProvider::CPU);
}

TEST(MatteEngineTest, ResolveProviderDirectMLWindowsOnly)
{
    ActiveProvider ap = resolve_provider(ExecutionProvider::DirectML);
#if defined(_WIN32)
    EXPECT_EQ(ap, ActiveProvider::DirectML);
#else
    // DirectML not available on non-Windows → CPU.
    EXPECT_EQ(ap, ActiveProvider::CPU);
#endif
}

TEST(MatteEngineTest, ResolveProviderCoreMLMacOSOnly)
{
    ActiveProvider ap = resolve_provider(ExecutionProvider::CoreML);
#if defined(__APPLE__)
    EXPECT_EQ(ap, ActiveProvider::CoreML);
#else
    // CoreML not available on non-macOS → CPU.
    EXPECT_EQ(ap, ActiveProvider::CPU);
#endif
}

// ---------------------------------------------------------------------------
// Multi-model specs (Phase 2.6)
// ---------------------------------------------------------------------------

TEST(MatteEngineMultiModelTest, AllSevenModelSpecsPresent)
{
    const auto &specs = getAllModelSpecs();
    ASSERT_EQ(specs.size(), static_cast<size_t>(7));
}

TEST(MatteEngineMultiModelTest, RvmSpecIsStatefulBchwImageNet)
{
    const auto &spec = getModelSpec(MatteModelType::RVM);
    EXPECT_EQ(spec.type, MatteModelType::RVM);
    EXPECT_TRUE(spec.is_stateful);
    EXPECT_EQ(spec.layout, TensorLayout::BCHW);
    EXPECT_EQ(spec.normalize, NormalizeMode::ImageNet);
    EXPECT_EQ(spec.postprocess, PostprocessMode::Direct);
    EXPECT_EQ(spec.output_channels, 1);
    EXPECT_EQ(spec.default_width, 320);
    EXPECT_EQ(spec.default_height, 192);
    EXPECT_EQ(spec.name, "rvm");
    EXPECT_EQ(spec.default_filename, "rvm_mobilenetv3_fp32.onnx");
}

TEST(MatteEngineMultiModelTest, MediaPipeSpecIsBhwcDivide255Channel1)
{
    const auto &spec = getModelSpec(MatteModelType::MediaPipe);
    EXPECT_EQ(spec.type, MatteModelType::MediaPipe);
    EXPECT_FALSE(spec.is_stateful);
    EXPECT_EQ(spec.layout, TensorLayout::BHWC);
    EXPECT_EQ(spec.normalize, NormalizeMode::Divide255);
    EXPECT_EQ(spec.postprocess, PostprocessMode::Channel1);
    EXPECT_EQ(spec.output_channels, 2);
    EXPECT_EQ(spec.default_width, 256);
    EXPECT_EQ(spec.default_height, 256);
}

TEST(MatteEngineMultiModelTest, SelfieSpecIsBhwcMinMax)
{
    const auto &spec = getModelSpec(MatteModelType::Selfie);
    EXPECT_EQ(spec.layout, TensorLayout::BHWC);
    EXPECT_EQ(spec.normalize, NormalizeMode::Divide255);
    EXPECT_EQ(spec.postprocess, PostprocessMode::MinMax);
    EXPECT_EQ(spec.output_channels, 1);
}

TEST(MatteEngineMultiModelTest, SelfieMulticlassSpecIsArgmax)
{
    const auto &spec = getModelSpec(MatteModelType::SelfieMulticlass);
    EXPECT_EQ(spec.postprocess, PostprocessMode::ArgmaxMulticlass);
    EXPECT_EQ(spec.output_channels, 6);
    EXPECT_EQ(spec.layout, TensorLayout::BHWC);
}

TEST(MatteEngineMultiModelTest, PPHumanSegSpecIsBchwChannel1MinMax)
{
    const auto &spec = getModelSpec(MatteModelType::PPHumanSeg);
    EXPECT_EQ(spec.layout, TensorLayout::BCHW);
    EXPECT_EQ(spec.normalize, NormalizeMode::PPHumanSeg);
    EXPECT_EQ(spec.postprocess, PostprocessMode::Channel1MinMax);
    EXPECT_EQ(spec.output_channels, 2);
    EXPECT_EQ(spec.default_width, 192);
    EXPECT_EQ(spec.default_height, 192);
}

TEST(MatteEngineMultiModelTest, SINetSpecIsBchwSinetNormChannel1)
{
    const auto &spec = getModelSpec(MatteModelType::SINet);
    EXPECT_EQ(spec.layout, TensorLayout::BCHW);
    EXPECT_EQ(spec.normalize, NormalizeMode::SINet);
    EXPECT_EQ(spec.postprocess, PostprocessMode::Channel1);
    EXPECT_EQ(spec.output_channels, 2);
    EXPECT_EQ(spec.default_width, 320);
    EXPECT_EQ(spec.default_height, 320);
}

TEST(MatteEngineMultiModelTest, TCMonoDepthSpecIsBchwNoNormMinMax)
{
    const auto &spec = getModelSpec(MatteModelType::TCMonoDepth);
    EXPECT_EQ(spec.layout, TensorLayout::BCHW);
    EXPECT_EQ(spec.normalize, NormalizeMode::None);
    EXPECT_EQ(spec.postprocess, PostprocessMode::MinMax);
    EXPECT_EQ(spec.output_channels, 1);
}

// ---------------------------------------------------------------------------
// resolveModelType (Phase 2.6)
// ---------------------------------------------------------------------------

TEST(MatteEngineMultiModelTest, ResolveModelTypeRvm)
{
    EXPECT_EQ(resolveModelType("rvm_mobilenetv3_fp32.onnx"), MatteModelType::RVM);
    EXPECT_EQ(resolveModelType("RVM_MobileNetV3_FP32.onnx"), MatteModelType::RVM);
    EXPECT_EQ(resolveModelType("/path/to/rvm_model.onnx"), MatteModelType::RVM);
    EXPECT_EQ(resolveModelType("C:\\models\\rvm.onnx"), MatteModelType::RVM);
}

TEST(MatteEngineMultiModelTest, ResolveModelTypeMediapipe)
{
    EXPECT_EQ(resolveModelType("mediapipe.with_runtime_opt.ort"), MatteModelType::MediaPipe);
    EXPECT_EQ(resolveModelType("MediaPipe.onnx"), MatteModelType::MediaPipe);
}

TEST(MatteEngineMultiModelTest, ResolveModelTypeSelfieMulticlassBeforeSelfie)
{
    // "selfie_multiclass" must be checked BEFORE "selfie_segmentation"
    EXPECT_EQ(resolveModelType("selfie_multiclass_256x256.ort"), MatteModelType::SelfieMulticlass);
    EXPECT_EQ(resolveModelType("selfie_segmentation.ort"), MatteModelType::Selfie);
}

TEST(MatteEngineMultiModelTest, ResolveModelTypePpHumanSeg)
{
    EXPECT_EQ(resolveModelType("pphumanseg_fp32.ort"), MatteModelType::PPHumanSeg);
}

TEST(MatteEngineMultiModelTest, ResolveModelTypeSinet)
{
    EXPECT_EQ(resolveModelType("SINet_Softmax_simple.ort"), MatteModelType::SINet);
}

TEST(MatteEngineMultiModelTest, ResolveModelTypeTcMonoDepth)
{
    EXPECT_EQ(resolveModelType("tcmonodepth_tcsmallnet_192x320.ort"), MatteModelType::TCMonoDepth);
}

TEST(MatteEngineMultiModelTest, ResolveModelTypeDefaultsToRvm)
{
    EXPECT_EQ(resolveModelType("unknown_model.onnx"), MatteModelType::RVM);
    EXPECT_EQ(resolveModelType(""), MatteModelType::RVM);
}

// ---------------------------------------------------------------------------
// Multi-model preprocessing (Phase 2.6)
// ---------------------------------------------------------------------------

TEST(MatteEngineMultiModelTest, PreprocessMediaPipeIsBhwcDivide255)
{
    // 1x1 white pixel → MediaPipe BHWC /255 → [1.0, 1.0, 1.0] interleaved HWC
    uint8_t white[4] = {255, 255, 255, 255};
    ImageFrame input{1, 1, white};

    auto out = preprocess_rgba_for_model(input, 1, 1,
        TensorLayout::BHWC, NormalizeMode::Divide255);
    ASSERT_EQ(out.size(), static_cast<size_t>(3));
    // BHWC = interleaved HWC: [R, G, B]
    EXPECT_NEAR(out[0], 1.0f, 0.01f);
    EXPECT_NEAR(out[1], 1.0f, 0.01f);
    EXPECT_NEAR(out[2], 1.0f, 0.01f);
}

TEST(MatteEngineMultiModelTest, PreprocessRvmIsBchwImageNet)
{
    // 1x1 white pixel → RVM BCHW ImageNet → 3 planes, each (1.0 - mean) / std
    uint8_t white[4] = {255, 255, 255, 255};
    ImageFrame input{1, 1, white};

    auto out = preprocess_rgba_for_model(input, 1, 1,
        TensorLayout::BCHW, NormalizeMode::ImageNet);
    ASSERT_EQ(out.size(), static_cast<size_t>(3));
    // BCHW: plane R [0], plane G [1], plane B [2]
    float expected_r = (1.0f - 0.485f) / 0.229f;
    float expected_g = (1.0f - 0.456f) / 0.224f;
    float expected_b = (1.0f - 0.406f) / 0.225f;
    EXPECT_NEAR(out[0], expected_r, 0.01f);
    EXPECT_NEAR(out[1], expected_g, 0.01f);
    EXPECT_NEAR(out[2], expected_b, 0.01f);
}

TEST(MatteEngineMultiModelTest, PreprocessTcMonoDepthIsBchwRaw255)
{
    // TCMonoDepth: no normalize → raw [0, 255]
    uint8_t pixel[4] = {100, 150, 200, 255};
    ImageFrame input{1, 1, pixel};

    auto out = preprocess_rgba_for_model(input, 1, 1,
        TensorLayout::BCHW, NormalizeMode::None);
    ASSERT_EQ(out.size(), static_cast<size_t>(3));
    EXPECT_NEAR(out[0], 100.0f, 0.1f);
    EXPECT_NEAR(out[1], 150.0f, 0.1f);
    EXPECT_NEAR(out[2], 200.0f, 0.1f);
}

TEST(MatteEngineMultiModelTest, PreprocessBhwcLayoutIsInterleaved)
{
    // 2x1 image: pixel0=red(255,0,0), pixel1=green(0,255,0)
    // BHWC layout = [R0, G0, B0, R1, G1, B1]
    std::vector<uint8_t> img(2 * 4);
    img[0] = 255; img[1] = 0;   img[2] = 0;   img[3] = 255;  // red
    img[4] = 0;   img[5] = 255; img[6] = 0;   img[7] = 255;  // green
    ImageFrame input{2, 1, img.data()};

    auto out = preprocess_rgba_for_model(input, 2, 1,
        TensorLayout::BHWC, NormalizeMode::Divide255);
    ASSERT_EQ(out.size(), static_cast<size_t>(6));
    EXPECT_NEAR(out[0], 1.0f, 0.01f);  // R0
    EXPECT_NEAR(out[1], 0.0f, 0.01f);  // G0
    EXPECT_NEAR(out[2], 0.0f, 0.01f);  // B0
    EXPECT_NEAR(out[3], 0.0f, 0.01f);  // R1
    EXPECT_NEAR(out[4], 1.0f, 0.01f);  // G1
    EXPECT_NEAR(out[5], 0.0f, 0.01f);  // B1
}

TEST(MatteEngineMultiModelTest, PreprocessBchwLayoutIsPlanar)
{
    // 2x1 image: pixel0=red, pixel1=green
    // BCHW layout = [R0, R1, G0, G1, B0, B1]
    std::vector<uint8_t> img(2 * 4);
    img[0] = 255; img[1] = 0;   img[2] = 0;   img[3] = 255;
    img[4] = 0;   img[5] = 255; img[6] = 0;   img[7] = 255;
    ImageFrame input{2, 1, img.data()};

    auto out = preprocess_rgba_for_model(input, 2, 1,
        TensorLayout::BCHW, NormalizeMode::Divide255);
    ASSERT_EQ(out.size(), static_cast<size_t>(6));
    // Plane R: [R0, R1]
    EXPECT_NEAR(out[0], 1.0f, 0.01f);
    EXPECT_NEAR(out[1], 0.0f, 0.01f);
    // Plane G: [G0, G1]
    EXPECT_NEAR(out[2], 0.0f, 0.01f);
    EXPECT_NEAR(out[3], 1.0f, 0.01f);
    // Plane B: [B0, B1]
    EXPECT_NEAR(out[4], 0.0f, 0.01f);
    EXPECT_NEAR(out[5], 0.0f, 0.01f);
}

// ---------------------------------------------------------------------------
// Postprocessing helpers (Phase 2.6)
// ---------------------------------------------------------------------------

TEST(MatteEngineMultiModelTest, NormalizeMinMaxRescalesTo01)
{
    // [10, 20, 30] → [0, 0.5, 1.0]
    float data[3] = {10.0f, 20.0f, 30.0f};
    auto out = normalize_minmax(data, 3);
    ASSERT_EQ(out.size(), static_cast<size_t>(3));
    EXPECT_NEAR(out[0], 0.0f, 0.01f);
    EXPECT_NEAR(out[1], 0.5f, 0.01f);
    EXPECT_NEAR(out[2], 1.0f, 0.01f);
}

TEST(MatteEngineMultiModelTest, NormalizeMinMaxFlatReturnsZeros)
{
    // Flat values → range=0 → all zeros
    float data[3] = {5.0f, 5.0f, 5.0f};
    auto out = normalize_minmax(data, 3);
    ASSERT_EQ(out.size(), static_cast<size_t>(3));
    for (float v : out) EXPECT_NEAR(v, 0.0f, 0.01f);
}

TEST(MatteEngineMultiModelTest, ArgmaxMulticlassForegroundWhenClassGtZero)
{
    // 1x1 pixel, 6 classes: class 0 (bg) = 0.9, class 3 (face) = 0.1
    // argmax → class 0 → background → mask = 0
    float data[6] = {0.9f, 0.0f, 0.0f, 0.1f, 0.0f, 0.0f};
    auto mask = argmax_multiclass_to_mask(data, 1, 1, 6);
    ASSERT_EQ(mask.size(), static_cast<size_t>(1));
    EXPECT_NEAR(mask[0], 0.0f, 0.01f);

    // Now class 3 (face) is max → foreground → mask = 0.1 (best_val)
    float data2[6] = {0.1f, 0.0f, 0.0f, 0.9f, 0.0f, 0.0f};
    auto mask2 = argmax_multiclass_to_mask(data2, 1, 1, 6);
    ASSERT_EQ(mask2.size(), static_cast<size_t>(1));
    EXPECT_NEAR(mask2[0], 0.9f, 0.01f);
}

} // namespace dancehap
