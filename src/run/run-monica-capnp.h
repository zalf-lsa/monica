/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
Authors: 
Michael Berg <michael.berg@zalf.de>

Maintainers: 
Currently maintained by the authors.

This file is part of the MONICA model. 
Copyright (C) Leibniz Centre for Agricultural Landscape Research (ZALF)
*/

#pragma once

#include <kj/debug.h>
#include <kj/common.h>

#include <capnp/rpc-twoparty.h>
#include <kj/thread.h>

#include "climate/climate-common.h"

#include "model.capnp.h"
#include "common.capnp.h"

namespace Monica
{
	class RunMonicaImpl final : public zalf::capnp::rpc::Model::EnvInstance::Server
	{
		// Implementation of the Model::Instance Cap'n Proto interface

		bool _startedServerInDebugMode{ false };
		
	public:
		RunMonicaImpl(bool startedServerInDebugMode = false) : _startedServerInDebugMode(startedServerInDebugMode) {}

		kj::Promise<void> run(RunContext context) override;
	};

}
