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

#include <iostream>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>
#include <algorithm>

#include <kj/debug.h>

#include <kj/common.h>
#define KJ_MVCAP(var) var = kj::mv(var)

#include <capnp/ez-rpc.h>
#include <capnp/message.h>

#include <capnp/rpc-twoparty.h>
#include <kj/thread.h>

#include "json11/json11.hpp"

#include "tools/helper.h"

#include "db/abstract-db-connections.h"
#include "tools/debug.h"
#include "run-monica.h"
#include "../core/monica-model.h"
#include "json11/json11-helper.h"
#include "climate/climate-file-io.h"
#include "soil/conversion.h"
#include "env-from-json-config.h"
#include "tools/algorithms.h"
#include "../io/csv-format.h"
#include "climate/climate-common.h"

#include "model.capnp.h"
#include "common.capnp.h"

#include "run-monica-capnp.h"

#include "common/sole.hpp"

//using namespace std;
using namespace Monica;
using namespace Tools;
using namespace json11;
using namespace Climate;
using namespace mas;

//std::map<std::string, DataAccessor> daCache;

DataAccessor fromCapnpData(
  const Tools::Date& startDate,
  const Tools::Date& endDate,
  capnp::List<rpc::Climate::Element>::Reader header,
  capnp::List<capnp::List<float>>::Reader data) {
  typedef rpc::Climate::Element E;

  if (data.size() == 0)
    return DataAccessor();

  DataAccessor da(startDate, endDate);
  //vector<double> d(data[0].size());
  for (int i = 0; i < header.size(); i++) {
    auto vs = data[i];
    std::vector<double> d(data[0].size());
    //transform(vs.begin(), vs.end(), d.begin(), [](float f) { return f; });
    for (int k = 0; k < vs.size(); k++)
      d[k] = vs[k];
    switch (header[i]) {
      case E::TMIN: da.addClimateData(ACD::tmin, std::move(d)); break;
      case E::TAVG: da.addClimateData(ACD::tavg, std::move(d)); break;
      case E::TMAX: da.addClimateData(ACD::tmax, std::move(d)); break;
      case E::PRECIP: da.addClimateData(ACD::precip, std::move(d)); break;
      case E::RELHUMID: da.addClimateData(ACD::relhumid, std::move(d)); break;
      case E::WIND: da.addClimateData(ACD::wind, std::move(d)); break;
      case E::GLOBRAD: da.addClimateData(ACD::globrad, std::move(d)); break;
      default:;
    }
  }

  return da;
}

J11Array fromCapnpSoilProfile(rpc::Soil::Profile::Reader profile) {
  J11Array ls;
  for (const auto& layer : profile.getLayers()) {
    J11Object l;
    l["Thickness"] = layer.getSize();
    for (const auto& prop : layer.getProperties()) {
      switch (prop.getName()) {
      case rpc::Soil::PropertyName::SAND: l["Sand"] = prop.getF32Value() / 100.0; break;
      case rpc::Soil::PropertyName::CLAY: l["Clay"] = prop.getF32Value() / 100.0; break;
      case rpc::Soil::PropertyName::SILT: l["Silt"] = prop.getF32Value() / 100.0; break;
      case rpc::Soil::PropertyName::ORGANIC_CARBON: l["SoilOrganicCarbon"] = prop.getF32Value(); break;
      case rpc::Soil::PropertyName::ORGANIC_MATTER: l["SoilOrganicMatter"] = prop.getF32Value() / 100.0; break;
      case rpc::Soil::PropertyName::BULK_DENSITY: l["SoilBulkDensity"] = prop.getF32Value(); break;
      case rpc::Soil::PropertyName::RAW_DENSITY: l["SoilRawDensity"] = prop.getF32Value(); break;
      case rpc::Soil::PropertyName::P_H: l["pH"] = prop.getF32Value(); break;
      case rpc::Soil::PropertyName::SOIL_TYPE: l["KA5TextureClass"] = prop.getType(); break;
      case rpc::Soil::PropertyName::PERMANENT_WILTING_POINT: l["PermanentWiltingPoint"] = prop.getF32Value() / 100.0; break;
      case rpc::Soil::PropertyName::FIELD_CAPACITY: l["FieldCapacity"] = prop.getF32Value() / 100.0; break;
      case rpc::Soil::PropertyName::SATURATION: l["PoreVolume"] = prop.getF32Value() / 100.0; break;
      case rpc::Soil::PropertyName::SOIL_WATER_CONDUCTIVITY_COEFFICIENT: l["Lambda"] = prop.getF32Value(); break;
      case rpc::Soil::PropertyName::SCELETON: l["Sceleton"] = prop.getF32Value() / 100.0; break;
      case rpc::Soil::PropertyName::AMMONIUM: l["SoilAmmonium"] = prop.getF32Value(); break;
      case rpc::Soil::PropertyName::NITRATE: l["SoilNitrate"] = prop.getF32Value(); break;
      case rpc::Soil::PropertyName::CN_RATIO: l["CN"] = prop.getF32Value(); break;
      case rpc::Soil::PropertyName::SOIL_MOISTURE: l["SoilMoisturePercentFC"] = prop.getF32Value(); break;
      case rpc::Soil::PropertyName::IN_GROUNDWATER: l["is_in_groundwater"] = prop.getBValue(); break;
      case rpc::Soil::PropertyName::IMPENETRABLE: l["is_impenetrable"] = prop.getBValue(); break;
      }
    }
    ls.push_back(l);
  }
  return ls;
}

kj::Promise<void> RunMonicaImpl::info(InfoContext context) //override
{
  auto rs = context.getResults();
  rs.setId("monica_" + sole::uuid4().str());
  rs.setName("Monica capnp server");
  rs.setDescription("");
  return kj::READY_NOW;
}

kj::Promise<void> RunMonicaImpl::run(RunContext context) //override
{
  debug() << ".";

  auto envR = context.getParams().getEnv();

  auto runMonica = [context, envR, this](DataAccessor da = DataAccessor()) mutable {
    std::string err;
    auto rest = envR.getRest();
    if (!rest.getStructure().isJson()) {
      return Monica::Output(std::string("Error: 'rest' field is not valid JSON!"));
    }

    const Json& envJson = Json::parse(rest.getValue().cStr(), err);
    //cout << "runMonica: " << envJson["customId"].dump() << endl;

    Env env;
    auto errors = env.merge(envJson);

    //if we got a capnproto soil profile in the message, overwrite an existing profile by that one
    if (envR.hasSoilProfile()) {
      auto layers = fromCapnpSoilProfile(envR.getSoilProfile());
      env.params.siteParameters.merge(J11Object{ {"SoilProfileParameters", layers} });
    }

    EResult<DataAccessor> eda;
    if (da.isValid()) {
      eda.result = da;
    } else if (!env.climateData.isValid()) {
      if (!env.climateCSV.empty()) {
        eda = readClimateDataFromCSVStringViaHeaders(env.climateCSV, env.csvViaHeaderOptions);
      } else if (!env.pathsToClimateCSV.empty()) {
        eda = readClimateDataFromCSVFilesViaHeaders(env.pathsToClimateCSV, env.csvViaHeaderOptions);
      }
    }

    Monica::Output out;
    if (eda.success()) {
      env.climateData = eda.result;

      env.debugMode = _startedServerInDebugMode && env.debugMode;

      env.params.userSoilMoistureParameters.getCapillaryRiseRate =
        [](std::string soilTexture, size_t distance) {
        return Soil::readCapillaryRiseRates().getRate(soilTexture, distance);
      };

      out = Monica::runMonica(env);
    }

    out.errors = eda.errors;
    out.warnings = eda.warnings;

    return out;
  };

  if (envR.hasTimeSeries()) {
    auto ts = envR.getTimeSeries();
    auto rangeProm = ts.rangeRequest().send();
    auto headerProm = ts.headerRequest().send();
    auto dataTProm = ts.dataTRequest().send();

    return rangeProm
      .then([KJ_MVCAP(headerProm), KJ_MVCAP(dataTProm)](auto&& rangeResponse) mutable {
      return headerProm
        .then([KJ_MVCAP(rangeResponse), KJ_MVCAP(dataTProm)](auto&& headerResponse) mutable {
        return dataTProm
          .then([KJ_MVCAP(rangeResponse), KJ_MVCAP(headerResponse)](auto&& dataTResponse) mutable {
          auto sd = rangeResponse.getStartDate();
          auto ed = rangeResponse.getEndDate();
          return fromCapnpData(
            Tools::Date(sd.getDay(), sd.getMonth(), sd.getYear()),
            Tools::Date(ed.getDay(), ed.getMonth(), ed.getYear()),
            headerResponse.getHeader(), dataTResponse.getData());
        });
      });
    }).then([context, runMonica](DataAccessor da) mutable {
      auto out = runMonica(da);
      auto rs = context.getResults();
      rs.initResult();
      rs.getResult().setValue(out.toString());
            });
  } else {
    auto out = runMonica();
    auto rs = context.getResults();
    rs.initResult();
    rs.getResult().setValue(out.toString());
    return kj::READY_NOW;
  }
}

kj::Promise<void> RunMonicaImpl::stop(StopContext context) //override
{
  std::cout << "Stop received. Exiting. cout" << std::endl;
  KJ_LOG(INFO, "Stop received. Exiting.");
  return unregister.callRequest().send().then([](auto&&) { 
    std::cout << "exit(0)" << std::endl;
    exit(0); 
                                              });
}