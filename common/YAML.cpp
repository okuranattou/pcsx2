// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "YAML.h"

#if __has_include("c4/yml/error.hpp")
#include "c4/yml/error.hpp"
#define PCSX2_RYML_HAS_SPLIT_ERROR_CALLBACKS 1
#endif

#include <csetjmp>
#include <cstdlib>

struct RapidYAMLContext
{
	std::jmp_buf env;
	Error* error = nullptr;
};

std::optional<ryml::Tree> ParseYAMLFromString(ryml::csubstr yaml, ryml::csubstr file_name, Error* error)
{
	RapidYAMLContext context;
	context.error = error;

	ryml::Callbacks callbacks;
#if PCSX2_RYML_HAS_SPLIT_ERROR_CALLBACKS
	callbacks.set_user_data(static_cast<void*>(&context));
	callbacks.set_error_basic([](ryml::csubstr msg, ryml::ErrorDataBasic const& errdata, void* user_data) {
		RapidYAMLContext* context = static_cast<RapidYAMLContext*>(user_data);

		Error::SetString(context->error, std::string(msg.str, msg.len));
		std::longjmp(context->env, 1);
	});
	callbacks.set_error_parse([](ryml::csubstr msg, ryml::ErrorDataParse const& errdata, void* user_data) {
		RapidYAMLContext* context = static_cast<RapidYAMLContext*>(user_data);

		Error::SetString(context->error, std::string(msg.str, msg.len));
		std::longjmp(context->env, 1);
	});
#else
	callbacks.m_user_data = static_cast<void*>(&context);
	callbacks.m_error = [](const char* msg, size_t msg_len, ryml::Location location, void* user_data) {
		RapidYAMLContext* context = static_cast<RapidYAMLContext*>(user_data);

		Error::SetString(context->error, std::string(msg, msg_len));
		std::longjmp(context->env, 1);
	};
#endif

	ryml::EventHandlerTree event_handler(callbacks);
	ryml::Parser parser(&event_handler);

	ryml::Tree tree;

	// The only options RapidYAML provides for recovering from errors are
	// throwing an exception or using setjmp/longjmp. Since we have exceptions
	// disabled we have to use the latter option.
	if (setjmp(context.env))
		return std::nullopt;

	ryml::parse_in_arena(&parser, file_name, yaml, &tree);

	return tree;
}
