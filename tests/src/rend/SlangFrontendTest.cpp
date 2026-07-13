/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "rend/postprocess/slang_frontend.h"

#include <gtest/gtest.h>

TEST(SlangFrontendTest, ParsesTypedParametersStagesAndConventionalResources)
{
	const std::string source =
		"#version 450\n"
		"#pragma name \"CRT\"\n"
		"#pragma parameter float BLOOM \"Bloom strength\" 0.5 0.0 1.0 0.05\n"
		"#pragma parameter vec3 TINT \"Tint\" vec3(1, 0.9, 0.8) vec3(0, 0, 0) vec3(1, 1, 1) vec3(0.1, 0.1, 0.1)\n"
		"uniform sampler2D Source;\n"
		"uniform sampler2D Original;\n"
		"uniform sampler2D PassAHistory;\n"
		"uniform sampler2D PassAFeedback;\n"
		"uniform sampler3D LUT;\n"
		"#pragma stage vertex\n"
		"void CustomVertex() {}\n"
		"#pragma stage fragment\n"
		"void main() {}\n";

	postprocess::SlangShaderIR shader;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangFrontend(source, shader, error)) << error;
	EXPECT_EQ("CRT", shader.displayName);
	ASSERT_EQ(2u, shader.parameters.size());
	EXPECT_EQ(postprocess::SlangParameterType::Float3, shader.parameters[1].type);
	EXPECT_FLOAT_EQ(0.9f, shader.parameters[1].defaultValue.values[1]);
	ASSERT_EQ(5u, shader.resources.size());
	EXPECT_EQ(postprocess::SlangResourceConvention::Source, shader.resources[0].convention);
	EXPECT_EQ(postprocess::SlangResourceConvention::Original, shader.resources[1].convention);
	EXPECT_EQ(postprocess::SlangResourceConvention::History, shader.resources[2].convention);
	EXPECT_EQ("PassA", shader.resources[2].alias);
	EXPECT_EQ(postprocess::SlangResourceConvention::Feedback, shader.resources[3].convention);
	EXPECT_NE(std::string::npos, shader.vertex.source.find("CustomVertex"));
	EXPECT_NE(std::string::npos, shader.fragment.source.find("void main"));
}

TEST(SlangFrontendTest, RejectsMalformedParametersAndResources)
{
	postprocess::SlangShaderIR shader;
	std::string error;
	EXPECT_FALSE(postprocess::parseSlangFrontend(
			"#version 450\n#pragma parameter int STEPS \"Steps\" 1.5 0 4 1\nvoid main() {}\n", shader, error));
	EXPECT_NE(std::string::npos, error.find("parameter"));
	EXPECT_FALSE(postprocess::parseSlangFrontend(
			"#version 450\nuniform sampler2D Broken[0];\nvoid main() {}\n", shader, error));
	EXPECT_NE(std::string::npos, error.find("resource"));
}

TEST(SlangFrontendTest, PreservesStandardExplicitResourceBindings)
{
	postprocess::SlangShaderIR shader;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangFrontend(
			"#version 450\n"
			"layout(set = 2, binding = 7) uniform sampler2D Source;\n"
			"layout(binding = 8) uniform sampler2DArray PassA[2];\n"
			"void main() {}\n", shader, error)) << error;
	ASSERT_EQ(2u, shader.resources.size());
	EXPECT_TRUE(shader.resources[0].hasExplicitBinding);
	EXPECT_EQ(2u, shader.resources[0].bindingSet);
	EXPECT_EQ(7u, shader.resources[0].binding);
	EXPECT_EQ(8u, shader.resources[1].binding);
	EXPECT_EQ(2u, shader.resources[1].arrayCount);
}
