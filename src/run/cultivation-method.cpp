/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
Authors:
Michael Berg <michael.berg@zalf.de>
Claas Nendel <claas.nendel@zalf.de>
Xenia Specka <xenia.specka@zalf.de>

Maintainers:
Currently maintained by the authors.

This file is part of the MONICA model.
Copyright (C) Leibniz Centre for Agricultural Landscape Research (ZALF)
*/

#include <map>
#include <sstream>
#include <iostream>
#include <fstream>
#include <cmath>
#include <utility>
#include <mutex>

#include "db/abstract-db-connections.h"
#include "climate/climate-common.h"
#include "tools/helper.h"
#include "tools/json11-helper.h"
#include "tools/algorithms.h"
#include "../core/monica-parameters.h"
#include "../core/monica-model.h"
#include "tools/debug.h"
#include "soil/conversion.h"
#include "soil/soil.h"
#include "../io/database-io.h"
#include "../io/build-output.h"

#include "cultivation-method.h"

using namespace Db;
using namespace std;
using namespace Monica;
using namespace Soil;
using namespace Tools;
using namespace Climate;


std::pair<Date, bool> makeInitAbsDate(Date date, Date initDate, bool addYear)
{
	bool addedYear = false;

	if(date.isAbsoluteDate())
		return make_pair(date, addedYear);

	Date absDate = date.toAbsoluteDate(initDate.year());
	if(addYear || absDate < initDate)
	{
		addedYear = true;
		absDate.addYears(1);
	}

	return make_pair(absDate, addedYear);
}

//----------------------------------------------------------------------------

Workstep::Workstep(const Tools::Date& d)
	: _date(d)
{}

Workstep::Workstep(size_t noOfDaysAfterEvent, const std::string& afterEvent)
	: _applyNoOfDaysAfterEvent(noOfDaysAfterEvent)
	, _afterEvent(afterEvent)
{}

Workstep::Workstep(json11::Json j)
{
	merge(j);
}

Errors Workstep::merge(json11::Json j)
{
	Errors res = Json11Serializable::merge(j);

	set_iso_date_value(_date, j, "date");
	set_int_value(_applyNoOfDaysAfterEvent, j, "days");
	set_string_value(_afterEvent, j, "after");

	return res;
}

json11::Json Workstep::to_json() const
{
	return json11::Json::object{
		{"type", type()},
		{"date", date().toIsoDateString()},};
}

bool Workstep::apply(MonicaModel* model)
{
	model->addEvent("Workstep");
	return true;
}

bool Workstep::applyWithPossibleCondition(MonicaModel* model)
{
	bool workstepFinished = false;
	if(isActive())
	{
		if(isDynamicWorkstep())
			workstepFinished = condition(model) ? apply(model) : false;
		else
			workstepFinished = apply(model);
		_isActive = !workstepFinished;
	}
	return workstepFinished;
}

bool Workstep::condition(MonicaModel* model)
{
	if(_afterEvent.empty())
		return false;

	auto currEvents = model->currentEvents();
	auto prevEvents = model->previousDaysEvents();
	
	auto ceit = currEvents.find(_afterEvent);
	if(_daysAfterEventCount >= 0)
		_daysAfterEventCount++;
	else if(ceit != currEvents.end()
					|| prevEvents.find(_afterEvent) != prevEvents.end())
		_daysAfterEventCount = 0;

	return _daysAfterEventCount == _applyNoOfDaysAfterEvent;
}

bool Workstep::reinit(Tools::Date date, bool addYear)
{ 
	bool addedYear = false;

	if(_date.isValid())
		tie(_absDate, addedYear) = makeInitAbsDate(_date, date, addYear);

	_isActive = true;
	_daysAfterEventCount = -1;

	return addedYear;
}

//------------------------------------------------------------------------------

Sowing::Sowing(const Tools::Date& at, CropPtr crop)
	: Workstep(at)
	, _crop(crop)
{
	if(_crop)
		_crop->setSeedDate(at);
}

Sowing::Sowing(json11::Json j)
{
	merge(j);
}

Errors Sowing::merge(json11::Json j)
{
	Errors res = Workstep::merge(j);
	set_shared_ptr_value(_crop, j, "crop");
	if(_crop)
	{
		_crop->setSeedDate(date());
		set_int_value(_plantDensity, j, "PlantDensity");
		if(_plantDensity > 0)
			_crop->cropParameters()->speciesParams.pc_PlantDensity = _plantDensity;
	}

	return res;
}

json11::Json Sowing::to_json(bool includeFullCropParameters) const
{
	auto o = json11::Json::object
	{{"type", type()}
	,{"date", date().toIsoDateString()}
	,{"crop", _crop ? _crop->to_json(includeFullCropParameters) : json11::Json()}};

	if(_plantDensity > 0)
		o["PlantDensity"] = J11Array{_plantDensity, "plants m-2"};

	return o;
}

bool Sowing::apply(MonicaModel* model)
{
	Workstep::apply(model);

	debug() << "sowing crop: " << _crop->toString() << " at: " << date().toString() << endl;
	model->seedCrop(_crop);
	model->addEvent("Sowing");

	return true;
}

//------------------------------------------------------------------------------

AutomaticSowing::AutomaticSowing(json11::Json j)
{
	merge(j);
}

Errors AutomaticSowing::merge(json11::Json j)
{
	Errors res = Workstep::merge(j);

	set_iso_date_value(_earliestDate, j, "earliest-date");
	set_iso_date_value(_latestDate, j, "latest-date");
	set_double_value(_minTempThreshold, j, "min-temp");
	set_int_value(_daysInTempWindow, j, "days-in-temp-window");
	set_double_value(_minPercentASW, j, "min-%-asw");
	set_double_value(_maxPercentASW, j, "max-%-asw");
	set_double_value(_max3dayPrecipSum, j, "max-3d-precip");
	set_double_value(_maxCurrentDayPrecipSum, j, "max-curr-day-precip");
	set_double_value(_tempSumAboveBaseTemp, j, "temp-sum-above-base-temp");
	set_double_value(_baseTemp, j, "base-temp");

	set_shared_ptr_value(_crop, j, "crop");
	if(_crop)
		_crop->setSeedDate(date());

	return res;
}

json11::Json AutomaticSowing::to_json(bool includeFullCropParameters) const
{
	return json11::Json::object
	{{"type", type()}
	,{"earliest-date", J11Array{_earliestDate.toIsoDateString(), "", "earliest sowing date"}}
	,{"latest-date", J11Array{_latestDate.toIsoDateString(), "", "latest sowing date"}}
	,{"min-temp", J11Array{_minTempThreshold, "�C", "minimal air temperature for sowing (T >= thresh && avg T in Twindow >= thresh)"}}
	,{"days-in-temp-window", J11Array{_daysInTempWindow, "d", "days to be used for sliding window of min-temp"}}
	,{"min-%-asw", J11Array{_minPercentASW, "%", "minimal soil-moisture in percent of available soil-water"}}
	,{"max-%-asw", J11Array{_maxPercentASW, "%", "maximal soil-moisture in percent of available soil-water"}}
	,{"max-3d-precip-sum", J11Array{_max3dayPrecipSum, "mm", "sum of precipitation in the last three days (including current day)"}}
	,{"max-curr-day-precip", J11Array{_maxCurrentDayPrecipSum, "mm", "max precipitation allowed at current day"}}
	,{"temp-sum-above-base-temp", J11Array{_tempSumAboveBaseTemp, "�C", "temperature sum above T-base needed"}}
	,{"base-temp", J11Array{_baseTemp, "�C", "base temperature above which temp-sum-above-base-temp is counted"}}
	,{"is-winter-crop", _crop ? _crop->isWinterCrop() : json11::Json()}
	,{"crop", _crop ? _crop->to_json(includeFullCropParameters) : json11::Json()}
	};
}

bool isSoilMoistureOk(MonicaModel* model, 
											double minPercentASW, 
											double maxPercentASW)
{
	bool soilMoistureOk = false;
	double pwp = model->soilColumn().at(0).vs_PermanentWiltingPoint();
	double sm = max(0.0, model->soilColumn().at(0).get_Vs_SoilMoisture_m3() - pwp);
	double asw = model->soilColumn().at(0).vs_FieldCapacity() - pwp;
	double currentPercentASW = sm / asw * 100.0;
	soilMoistureOk = minPercentASW <= currentPercentASW && currentPercentASW <= maxPercentASW;

	return soilMoistureOk;
}

bool isPrecipitationOk(const std::vector<std::map<Climate::ACD, double>>& climateData, 
											 double max3dayPrecipSum, 
											 double maxCurrentDayPrecipSum)
{
	bool precipOk = false;
	double psum3d = accumulate(climateData.rbegin(), climateData.rbegin() + 3, 0.0,
														 [](double acc, const map<ACD, double>& d)
	{
		auto it = d.find(Climate::precip);
		return acc + (it == d.end() ? 0 : it->second);
	});
	double currentp = climateData.back().at(Climate::precip);
	precipOk = psum3d <= max3dayPrecipSum && currentp <= maxCurrentDayPrecipSum;

	return precipOk;
}

bool AutomaticSowing::apply(MonicaModel* model)
{
	auto currentDate = model->currentStepDate();

	setDate(currentDate);
	_crop->setSeedDate(currentDate);

	Sowing::apply(model);
	model->addEvent("AutomaticSowing");
	_cropSeeded = true;
	_inSowingRange = false;

	return true;
}

bool AutomaticSowing::condition(MonicaModel* model)
{
	if(_cropSeeded)
		return false;
	
	auto currentDate = model->currentStepDate();

	if(!_inSowingRange && currentDate < _absEarliestDate)
		return false;
	else
		_inSowingRange = true;

	if(_inSowingRange && currentDate >= _absLatestDate)
		return true;

	const auto& cd = model->climateData();
	auto currentCd = cd.back();

	auto avg = [&](Climate::ACD acd)
	{
		return accumulate(cd.rbegin(), cd.rbegin() + _daysInTempWindow, 0.0,
											[acd](double acc, const map<ACD, double>& d)
		{
			auto it = d.find(acd);
			return acc + (it == d.end() ? 0 : it->second);
		}) / min(int(cd.size()), _daysInTempWindow);
	};

	//check temperature
	bool Tok = false;
	if(_crop->isWinterCrop())
	{
		double avgTavg = avg(Climate::tavg);
		Tok = avgTavg <= _minTempThreshold;
	}
	else
	{
		double avgTmin = avg(Climate::tmin);
		bool avgTminOk = avgTmin >= _minTempThreshold;
		bool TminOk = currentCd[Climate::tmin] >= _minTempThreshold;
		Tok = avgTminOk && TminOk;
	}

	if(!Tok)
		return false;

	//check soil moisture
	if(!isSoilMoistureOk(model, _minPercentASW, _maxPercentASW))
		return false;

	//check precipitation
	if(!isPrecipitationOk(cd, _max3dayPrecipSum, _maxCurrentDayPrecipSum))
		return false;

	//check temperature sum
	double baseTemp = _baseTemp;
	double tempSum = accumulate(cd.begin(), cd.end(), 0.0,
															[baseTemp](double acc, const map<ACD, double>& d)
	{
		auto it = d.find(Climate::tavg);
		return acc + (it == d.end() ? 0 : max(0.0, it->second - baseTemp));
	});
	if(tempSum < _tempSumAboveBaseTemp)
		return false;

	return true;
}

void AutomaticSowing::setDate(Tools::Date date)
{
	this->_date = date;
	_crop->setSeedAndHarvestDate(date, _crop->harvestDate());
}

bool AutomaticSowing::reinit(Tools::Date date, bool addYear)
{
	Workstep::reinit(date, addYear);

	_cropSeeded = _inSowingRange = false;
	setDate(Tools::Date());

	bool addedYear1, addedYear2;
	tie(_absEarliestDate, addedYear1) = makeInitAbsDate(_earliestDate, date, addYear);
	tie(_absLatestDate, addedYear2) = makeInitAbsDate(_latestDate, date, addYear);

	return addedYear1 || addedYear2;
}


//------------------------------------------------------------------------------

Harvest::Harvest()
	: _method("total")
{}

Harvest::Harvest(const Tools::Date& at,
								 CropPtr crop,
								 std::string method)
	: Workstep(at)
	, _crop(crop)
	, _method(method)
{
	if(_crop)
		_crop->setHarvestDate(at);
}

Harvest::Harvest(json11::Json j)
	: _method("total")
{
	merge(j);
}

Errors Harvest::merge(json11::Json j)
{
	Errors res = Workstep::merge(j);

	set_string_value(_method, j, "method");
	set_double_value(_percentage, j, "percentage");
	set_bool_value(_exported, j, "exported");

	return res;
}

json11::Json Harvest::to_json(bool includeFullCropParameters) const
{
	return json11::Json::object
	{{"type", type()}
	,{"date", date().toIsoDateString()}
//,{"crop", _crop ? _crop->to_json(includeFullCropParameters) :json11::Json()}
	,{"method", _method}
	,{"percentage", _percentage}
	,{ "exported", _exported }
	};
}

bool Harvest::apply(MonicaModel* model)
{
	Workstep::apply(model);

	if(model->cropGrowth())
	{
		auto crop = model->currentCrop();

		if(_method == "total"
			 || _method == "fruitHarvest"
			 || _method == "cutting")
		{
			debug() << "harvesting crop: " << crop->toString() << " at: " << date().toString() << endl;
			
			if(_method == "total")
				model->harvestCurrentCrop(_exported);
			else if(_method == "fruitHarvest")
				model->fruitHarvestCurrentCrop(_percentage, _exported);
			else if(_method == "cutting")
				model->cuttingCurrentCrop(_percentage, _exported);
		}
		else if(_method == "leafPruning")
		{
			debug() << "pruning leaves of: " << crop->toString() << " at: " << date().toString() << endl;
			model->leafPruningCurrentCrop(_percentage, _exported);
		}
		else if(_method == "tipPruning")
		{
			debug() << "pruning tips of: " << crop->toString() << " at: " << date().toString() << endl;
			model->tipPruningCurrentCrop(_percentage, _exported);
		}
		else if(_method == "shootPruning")
		{
			debug() << "pruning shoots of: " << crop->toString() << " at: " << date().toString() << endl;
			model->shootPruningCurrentCrop(_percentage, _exported);
		}
		model->addEvent("Harvest");
	}
	else
	{
		debug() << "Cannot harvest crop because there is not one anymore" << endl;
		debug() << "Maybe automatic harvest trigger was already activated so that the ";
		debug() << "crop was already harvested. This must be the fallback harvest application ";
		debug() << "that is not necessary anymore and should be ignored" << endl;
	}

	return true;
}

//------------------------------------------------------------------------------

AutomaticHarvest::AutomaticHarvest()
	: Harvest()
	, _harvestTime("maturity")
{}

AutomaticHarvest::AutomaticHarvest(CropPtr crop,
																				 std::string harvestTime,
																				 Date latestHarvest,
																				 std::string method)
	: Harvest(Date(), crop, method)
	, _harvestTime(harvestTime)
	, _latestDate(latestHarvest)
{}

AutomaticHarvest::AutomaticHarvest(json11::Json j)
	: Harvest(j)
	, _harvestTime("maturity")
{
	merge(j);
}

Errors AutomaticHarvest::merge(json11::Json j)
{
	Errors res = Harvest::merge(j);

	set_iso_date_value(_latestDate, j, "latest-date");
	set_double_value(_minPercentASW, j, "min-%-asw");
	set_double_value(_maxPercentASW, j, "max-%-asw");
	set_double_value(_max3dayPrecipSum, j, "max-3d-precip");
	set_double_value(_maxCurrentDayPrecipSum, j, "max-curr-day-precip");
	set_string_value(_harvestTime, j, "harvest-time");

	return res;
}

json11::Json AutomaticHarvest::to_json(bool includeFullCropParameters) const
{
	auto o = Harvest::to_json(includeFullCropParameters).object_items();
	o["type"] = type();
	o["latest-date"] = J11Array{_latestDate.toIsoDateString(), "", "latest harvesting date"};
	o["min-%-asw"] = J11Array{_minPercentASW, "%", "minimal soil-moisture in percent of available soil-water"};
	o["max-%-asw"], J11Array{_maxPercentASW, "%", "maximal soil-moisture in percent of available soil-water"};
	o["max-3d-precip-sum"], J11Array{_max3dayPrecipSum, "mm", "sum of precipitation in the last three days (including current day)"};
	o["max-curr-day-precip"], J11Array{_maxCurrentDayPrecipSum, "mm", "max precipitation allowed at current day"};
	o["harvest-time"] = _harvestTime;
	return o;
}

bool AutomaticHarvest::apply(MonicaModel* model)
{
	setDate(model->currentStepDate());
	
	Harvest::apply(model);
	
	model->addEvent("AutomaticHarvest");
	_cropHarvested = true;

	return true;
}

bool AutomaticHarvest::condition(MonicaModel* model)
{
	bool conditionMet = false;
	
	auto cg = model->cropGrowth();
	if(cg && !_cropHarvested) //got a crop and not yet harvested
		conditionMet =
			model->currentStepDate() >= _absLatestDate  //harvest after or at latested date
			|| (_harvestTime == "maturity" && model->cropGrowth()->maturityReached()) //has maturity been reached
			|| isSoilMoistureOk(model, _minPercentASW, _maxPercentASW)  //check soil moisture
			|| isPrecipitationOk(model->climateData(), _max3dayPrecipSum, _maxCurrentDayPrecipSum); //check precipitation

	return conditionMet;
}

bool AutomaticHarvest::reinit(Tools::Date date, bool addYear)
{
	Workstep::reinit(date, addYear);

	_cropHarvested = false;
	setDate(Tools::Date());

	bool addedYear;
	tie(_absLatestDate, addedYear) = makeInitAbsDate(_latestDate, date, addYear);

	return addedYear;
}


//------------------------------------------------------------------------------

Cutting::Cutting(const Tools::Date& at)
	: Workstep(at)
{}

Cutting::Cutting(json11::Json j)
	: Workstep(j)
{
	merge(j);
}

Errors Cutting::merge(json11::Json j)
{
	return Workstep::merge(j);
}

json11::Json Cutting::to_json() const
{
	return json11::Json::object{
		{"type", type()},
		{"date", date().toIsoDateString()}};
}

bool Cutting::apply(MonicaModel* model)
{
	Workstep::apply(model);

	assert(model->currentCrop() && model->cropGrowth());
	auto crop = model->currentCrop();
	debug() << "Cutting crop: " << crop->toString() << " at: " << date().toString() << endl;
	//crop->setHarvestYields(model->cropGrowth()->get_FreshPrimaryCropYield() / 100.0,
	//											 model->cropGrowth()->get_FreshSecondaryCropYield() / 100.0);

	//crop->setHarvestYieldsTM(model->cropGrowth()->get_PrimaryCropYield() / 100.0,
	//												 model->cropGrowth()->get_SecondaryCropYield() / 100.0);

	//crop->setYieldNContent(model->cropGrowth()->get_PrimaryYieldNContent(),
	//											 model->cropGrowth()->get_SecondaryYieldNContent());

	//crop->setSumTotalNUptake(model->cropGrowth()->get_SumTotalNUptake());
	//crop->setCropHeight(model->cropGrowth()->get_CropHeight());

	model->cropGrowth()->applyCutting();
	model->addEvent("Cutting");

	return true;
}

//------------------------------------------------------------------------------

MineralFertilization::
MineralFertilization(const Tools::Date& at,
														 MineralFertiliserParameters partition,
														 double amount)
	: Workstep(at)
	, _partition(partition)
	, _amount(amount)
{}

MineralFertilization::MineralFertilization(json11::Json j)
{
	merge(j);
}

Errors MineralFertilization::merge(json11::Json j)
{
	Errors res = Workstep::merge(j);
	set_value_obj_value(_partition, j, "partition");
	set_double_value(_amount, j, "amount");
	return res;
}

json11::Json MineralFertilization::to_json() const
{
	return json11::Json::object
	{{"type", type()}
	,{"date", date().toIsoDateString()}
	,{"amount", _amount}
	,{"partition", _partition}
	};
}

bool MineralFertilization::apply(MonicaModel* model)
{
	Workstep::apply(model);

	debug() << toString() << endl;
	model->applyMineralFertiliser(partition(), amount());
	model->addEvent("MineralFertilization");

	return true;
}

//------------------------------------------------------------------------------

NDemandFertilization::
NDemandFertilization(int stage,
									 double depth,
									 MineralFertiliserParameters partition,
									 double Ndemand)
	: Workstep(Date())
	, _partition(partition)
	, _Ndemand(Ndemand)
	, _depth(depth)
	, _stage(stage)
{}

NDemandFertilization::NDemandFertilization(Tools::Date date,
																			 double depth,
																			 MineralFertiliserParameters partition,
																			 double Ndemand)
	: Workstep(date)
	, _partition(partition)
	, _Ndemand(Ndemand)
	, _depth(depth)
{}

NDemandFertilization::NDemandFertilization(json11::Json j)
{
	merge(j);
}

Errors NDemandFertilization::merge(json11::Json j)
{
	Errors res = Workstep::merge(j);
	set_double_value(_Ndemand, j, "N-demand");
	set_value_obj_value(_partition, j, "partition");
	set_double_value(_depth, j, "depth");
	set_int_value(_stage, j, "stage");
	
	return res;
}

json11::Json NDemandFertilization::to_json() const
{
	auto o = J11Object 
	{{"type", type()}
	,{"N-demand", _Ndemand}
	,{"partition", _partition}
	,{"depth", J11Array{_depth, "m", "depth of Nmin measurement"}}
	};
	if(date().isValid())
		o["date"] = date().toIsoDateString();
	else
		o["stage"] = J11Array{_stage, "", "if this development stage is entered, the fertilizer will be applied"};
	
	return o;
}

bool NDemandFertilization::apply(MonicaModel* model)
{
	Workstep::apply(model);

	double rd = model->cropGrowth()->get_RootingDepth_m();
	debug() << toString() << endl;
	double appliedAmount = model->soilColumnNC().applyMineralFertiliserViaNDemand(partition(), rd < _depth ? rd : _depth, _Ndemand);
	model->addDailySumFertiliser(appliedAmount);
	_appliedFertilizer = true;
	setDate(model->currentStepDate());
	model->addEvent("NDemandFertilization");

	return true;
}

bool NDemandFertilization::condition(MonicaModel* model)
{
	bool conditionMet = false;

	auto cg = model->cropGrowth();
	if(cg && !_appliedFertilizer)
	{
		auto currStage = cg->get_DevelopmentalStage() + 1;
		conditionMet =
			date().isValid() //is timed application
			|| _stage == 0   //apply at seeding time
			|| currStage == _stage; //reached the requested stage
	}

	return conditionMet;
}

bool NDemandFertilization::reinit(Tools::Date date, bool addYear)
{
	bool addedYear = Workstep::reinit(date, addYear);

	_appliedFertilizer = false;

	return false;
}

//------------------------------------------------------------------------------

OrganicFertilization::
OrganicFertilization(const Tools::Date& at,
														 OrganicMatterParametersPtr params,
														 double amount,
														 bool incorp)
	: Workstep(at)
	, _params(params)
	, _amount(amount)
	, _incorporation(incorp)
{}

OrganicFertilization::OrganicFertilization(json11::Json j)
{
	merge(j);
}

Errors OrganicFertilization::merge(json11::Json j)
{
	Errors res = Workstep::merge(j);
	set_shared_ptr_value(_params, j, "parameters");
	set_double_value(_amount, j, "amount");
	set_bool_value(_incorporation, j, "incorporation");
	return res;
}

json11::Json OrganicFertilization::to_json() const
{
	return json11::Json::object{
		{"type", type()},
		{"date", date().toIsoDateString()},
		{"amount", _amount},
		{"parameters", _params ? _params->to_json() : ""},
		{"incorporation", _incorporation}};
}

bool OrganicFertilization::apply(MonicaModel* model)
{
	Workstep::apply(model);

	debug() << toString() << endl;
	model->applyOrganicFertiliser(_params, _amount, _incorporation);
	model->addEvent("OrganicFertilization");

	return true;
}

//------------------------------------------------------------------------------

Tillage::Tillage(const Tools::Date& at,
																			 double depth)
	: Workstep(at)
	, _depth(depth)
{}

Tillage::Tillage(json11::Json j)
{
	merge(j);
}

Errors Tillage::merge(json11::Json j)
{
	Errors res = Workstep::merge(j);
	set_double_value(_depth, j, "depth");
	return res;
}

json11::Json Tillage::to_json() const
{
	return json11::Json::object{
		{"type", type()},
		{"date", date().toIsoDateString()},
		{"depth", _depth}};
}

bool Tillage::apply(MonicaModel* model)
{
	Workstep::apply(model);

	debug() << toString() << endl;
	model->applyTillage(_depth);
	model->addEvent("Tillage");

	return true;
}

//------------------------------------------------------------------------------

SetValue::SetValue(const Tools::Date& at,
									 OId oid,
									 json11::Json value)
	: Workstep(at)
	, _oid(oid)
	, _value(value)
{}

SetValue::SetValue(json11::Json j)
{
	merge(j);
}

Errors SetValue::merge(json11::Json j)
{
	Errors res = Workstep::merge(j);

	auto oids = parseOutputIds({j["var"]});
	if(!oids.empty())
		_oid = oids[0];
	else
		return res;

	_value = j["value"];
	if(_value.is_array())
	{
		auto jva = _value.array_items();
		if(!jva.empty())
		{
			//is an expression
			if(jva[0] == "=" && jva.size() == 4)
			{
				auto f = buildPrimitiveCalcExpression(J11Array(jva.begin() + 1, jva.end()));
				_getValue = [=](const MonicaModel* mm){ return f(*mm); };
			}
			else
			{
				auto oids = parseOutputIds({_value});
				if(!oids.empty())
				{
					auto oid = oids[0];
					const auto& ofs = buildOutputTable().ofs;
					auto ofi = ofs.find(oid.id);
					if(ofi != ofs.end())
					{
						auto f = ofi->second;
						_getValue = [=](const MonicaModel* mm){ return f(*mm, oid); };
					}
				}
			}
		}
	}
	else
		_getValue = [=](const MonicaModel*){ return _value; };

	return res;
}

json11::Json SetValue::to_json() const
{
	return json11::Json::object
	{{"type", type()}
	,{"date", date().toIsoDateString()}
	,{"var", _oid.jsonInput}
	,{"value", _value}
	};
}

bool SetValue::apply(MonicaModel* model)
{
	Workstep::apply(model);

	if(!_getValue)
		return true;

	const auto& setfs = buildOutputTable().setfs;
	auto ci = setfs.find(_oid.id);
	if(ci != setfs.end())
	{
		auto v = _getValue(model);
		ci->second(*model, _oid, v);
	}

	model->addEvent("SetValue");

	return true;
}

//------------------------------------------------------------------------------

Irrigation::Irrigation(const Tools::Date& at,
																						 double amount,
																						 IrrigationParameters params)
	: Workstep(at)
	, _amount(amount)
	, _params(params)
{}

Irrigation::Irrigation(json11::Json j)
{
	merge(j);
}

Errors Irrigation::merge(json11::Json j)
{
	Errors res = Workstep::merge(j);
	set_double_value(_amount, j, "amount");
	set_value_obj_value(_params, j, "parameters");
	return res;
}

json11::Json Irrigation::to_json() const
{
	return json11::Json::object{
		{"type", type()},
		{"date", date().toIsoDateString()},
		{"amount", _amount},
		{"parameters", _params}};
}

bool Irrigation::apply(MonicaModel* model)
{
	Workstep::apply(model);

	//cout << toString() << endl;
	model->applyIrrigation(amount(), nitrateConcentration());
	model->addEvent("Irrigation");

	return true;
}

//------------------------------------------------------------------------------

WSPtr Monica::makeWorkstep(json11::Json j)
{
	string type = string_value(j["type"]);
	if(type == "Sowing"
		 || type == "Seed") //deprecated name
		return make_shared<Sowing>(j);
	else if(type == "AutomaticSowing")
		return make_shared<AutomaticSowing>(j);
	else if(type == "Harvest")
		return make_shared<Harvest>(j);
	else if(type == "AutomaticHarvest")
		return make_shared<AutomaticHarvest>(j);
	else if(type == "Cutting")
		return make_shared<Cutting>(j);
	else if(type == "MineralFertilization" 
					|| type == "MineralFertiliserApplication") //deprecated name
		return make_shared<MineralFertilization>(j);
	else if(type == "NDemandFertilization")
		return make_shared<NDemandFertilization>(j);
	else if(type == "OrganicFertilization" 
					|| type == "OrganicFertiliserApplication") //deprecated name
		return make_shared<OrganicFertilization>(j);
	else if(type == "Tillage" 
					|| type == "TillageApplication") //deprecated name
		return make_shared<Tillage>(j);
	else if(type == "Irrigation" 
					|| type == "IrrigationApplication") //deprecated name
		return make_shared<Irrigation>(j);
	else if(type == "SetValue")
		return make_shared<SetValue>(j);

	return WSPtr();
}

CultivationMethod::CultivationMethod(const string& name)
	: _name(name)
{}

CultivationMethod::CultivationMethod(CropPtr crop,
																		 const std::string& name)
	: _name(name.empty() ? crop->id() : name)
	, _crop(crop)
{
	debug() << "CultivationMethod: " << name.c_str() << endl;

	if(crop->seedDate().isValid())
		addApplication(Sowing(crop->seedDate(), _crop));

	if(crop->harvestDate().isValid())
	{
		debug() << "crop->harvestDate(): " << crop->harvestDate().toString().c_str() << endl;
		addApplication(Harvest(crop->harvestDate(), _crop));
	}

	for(Date cd : crop->getCuttingDates())
	{
		debug() << "Add cutting date: " << cd.toString() << endl;
		addApplication(Cutting(cd));
	}
}

CultivationMethod::CultivationMethod(json11::Json j)
{
	merge(j);
}

Errors CultivationMethod::merge(json11::Json j)
{
	Errors res;

	set_int_value(_customId, j, "customId");
	set_string_value(_name, j, "name");
	set_bool_value(_irrigateCrop, j, "irrigateCrop");

	for(auto wsj : j["worksteps"].array_items())
	{
		auto ws = makeWorkstep(wsj);
		if(!ws)
			continue;
		_allWorksteps.insert(make_pair(iso_date_value(wsj, "date"), ws));
		string wsType = ws->type();
		if(wsType == "Sowing")
		{
			if(Sowing* seed = dynamic_cast<Sowing*>(ws.get()))
			{
				_crop = seed->crop();
				if((_name.empty() || _name == "Fallow") && _crop)
				{
					_name = _crop->id();
				}
			}
		}
		else if(wsType == "AutomaticSowing")
		{
			if(AutomaticSowing* seed = dynamic_cast<AutomaticSowing*>(ws.get()))
			{
				_crop = seed->crop();
				if((_name.empty() || _name == "Fallow") && _crop)
				{
					_name = _crop->id();
				}
			}
		}
		else if(wsType == "Harvest" 
						|| wsType == "AutomaticHarvest")
		{
			if(Harvest* harvest = dynamic_cast<Harvest*>(ws.get()))
			{
				harvest->setCrop(_crop);
				_crop->setHarvestDate(harvest->date());
			}
		}
	}

	return res;
}

json11::Json CultivationMethod::to_json() const
{
	auto wss = J11Array();
	for(auto d2ws : _allWorksteps)
		wss.push_back(d2ws.second->to_json());

	return J11Object
	{{"type", "CultivationMethod"}
	,{"customId", _customId}
  ,{"name", _name}
  ,{"irrigateCrop", _irrigateCrop}
  ,{"worksteps", wss}
	};
}

void CultivationMethod::apply(const Date& date, 
															MonicaModel* model) const
{
	for(auto wsp : workstepsAt(date))
		wsp->apply(model);
}

void CultivationMethod::absApply(const Date& date,
																 MonicaModel* model) const
{
	for(auto wsp : absWorkstepsAt(date))
		wsp->apply(model);
}

void CultivationMethod::apply(MonicaModel* model) 
{
	auto& udws = _unfinishedDynamicWorksteps;
	udws.erase(remove_if(udws.begin(), udws.end(),
											 [model](WSPtr wsp)
	{
		return wsp->applyWithPossibleCondition(model);
	}), udws.end());
}

Date CultivationMethod::nextDate(const Date& date) const
{
	auto ci = _allWorksteps.upper_bound(date);
	return ci != _allWorksteps.end() ? ci->first : Date();
}

Date CultivationMethod::nextAbsDate(const Date& date) const
{
	auto ci = _absWorksteps.upper_bound(date);
	return ci != _absWorksteps.end() ? ci->first : Date();
}


vector<WSPtr> CultivationMethod::workstepsAt(const Date& date) const
{
	vector<WSPtr> apps;
	auto p = _allWorksteps.equal_range(date);
	while(p.first != p.second)
	{
		apps.push_back(p.first->second);
		p.first++;
	}
	return apps;
}

vector<WSPtr> CultivationMethod::absWorkstepsAt(const Date& date) const
{
	vector<WSPtr> apps;
	auto p = _absWorksteps.equal_range(date);
	while(p.first != p.second)
	{
		apps.push_back(p.first->second);
		p.first++;
	}
	return apps;
}


bool CultivationMethod::areOnlyAbsoluteWorksteps() const
{
	return all_of(_allWorksteps.begin(), _allWorksteps.end(),
								[](decltype(_allWorksteps)::value_type p)
	{ 
		return p.first.isValid() && p.first.isAbsoluteDate(); 
	});
}

vector<WSPtr> CultivationMethod::staticWorksteps() const
{
	vector<WSPtr> wss;
	for(auto p : _allWorksteps)
		if(p.first.isValid())
			wss.push_back(p.second);
	return wss;
}

vector<WSPtr> CultivationMethod::allDynamicWorksteps() const
{
	return workstepsAt(Date());
}

Date CultivationMethod::startDate() const
{
	if(_allWorksteps.empty())
		return Date();

	auto dynEarliestStart = Date();
	for(auto app : workstepsAt(Date()))
	{
		auto ed = app->earliestDate();
		if((ed.isValid()
				&& dynEarliestStart.isValid()
				&& ed < dynEarliestStart)
			 || (ed.isValid() 
					 && !dynEarliestStart.isValid()))
			dynEarliestStart = ed;
	}

	auto it = _allWorksteps.begin();
	while(it != _allWorksteps.end() && !it->first.isValid())
		it++;

	if(dynEarliestStart.isValid() && it != _allWorksteps.end())
		return dynEarliestStart < it->first ? dynEarliestStart : it->first;
	else if(dynEarliestStart.isValid())
		return dynEarliestStart;
	else if(it != _allWorksteps.end())
		return it->first;
	
	return Date();
}

Date CultivationMethod::absStartDate() const
{
	if(_absWorksteps.empty())
		return Date();

	auto dynEarliestStart = Date();
	for(auto app : absWorkstepsAt(Date()))
	{
		auto ed = app->absEarliestDate();
		if((ed.isValid()
				&& dynEarliestStart.isValid()
				&& ed < dynEarliestStart)
			 || (ed.isValid()
					 && !dynEarliestStart.isValid()))
			dynEarliestStart = ed;
	}

	auto it = _absWorksteps.begin();
	while(it != _absWorksteps.end() && !it->first.isValid())
		it++;

	if(dynEarliestStart.isValid() && it != _absWorksteps.end())
		return dynEarliestStart < it->first ? dynEarliestStart : it->first;
	else if(dynEarliestStart.isValid())
		return dynEarliestStart;
	else if(it != _absWorksteps.end())
		return it->first;

	return Date();
}

Date CultivationMethod::endDate() const
{
	if(_allWorksteps.empty())
		return Date();

	auto dynLatestEnd = Date();
	for(auto app : workstepsAt(Date()))
	{
		auto ed = app->latestDate();
		if((ed.isValid()
				&& dynLatestEnd.isValid()
				&& ed > dynLatestEnd)
			 || (ed.isValid()
					 && !dynLatestEnd.isValid()))
			dynLatestEnd = ed;
	}

	auto it = _allWorksteps.rbegin();
	while(it != _allWorksteps.rend() && !it->first.isValid())
		it++;

	if(dynLatestEnd.isValid() && it != _allWorksteps.rend())
		return dynLatestEnd > it->first ? dynLatestEnd : it->first;
	else if(dynLatestEnd.isValid())
		return dynLatestEnd;
	else if(it != _allWorksteps.rend())
		return it->first;

	return Date();
}

Date CultivationMethod::absEndDate() const
{
	if(_absWorksteps.empty())
		return Date();

	auto dynLatestEnd = Date();
	for(auto app : absWorkstepsAt(Date()))
	{
		auto ed = app->latestDate();
		if((ed.isValid()
				&& dynLatestEnd.isValid()
				&& ed > dynLatestEnd)
			 || (ed.isValid()
					 && !dynLatestEnd.isValid()))
			dynLatestEnd = ed;
	}

	auto it = _absWorksteps.rbegin();
	while(it != _absWorksteps.rend() && !it->first.isValid())
		it++;

	if(dynLatestEnd.isValid() && it != _absWorksteps.rend())
		return dynLatestEnd > it->first ? dynLatestEnd : it->first;
	else if(dynLatestEnd.isValid())
		return dynLatestEnd;
	else if(it != _absWorksteps.rend())
		return it->first;

	return Date();
}

std::string CultivationMethod::toString() const
{
	ostringstream s;
	s << "name: " << name()
		<< " start: " << startDate().toString()
		<< " end: " << endDate().toString() << endl;
	s << "worksteps:" << endl;
	for(auto p : _allWorksteps)
		s << "at: " << p.first.toString()
		<< " what: " << p.second->toString() << endl;
	return s.str();
}

void CultivationMethod::reinit(Tools::Date date)
{
	_absWorksteps.clear();
	_unfinishedDynamicWorksteps.clear();
	bool addedYear = false;
	for(auto p : _allWorksteps)
	{
		auto ws = p.second;
		addedYear = ws->reinit(date, addedYear) || addedYear;
		_absWorksteps.insert(make_pair(ws->absDate(), p.second));
		if(!ws->absDate().isValid())
			_unfinishedDynamicWorksteps.push_back(p.second);
	}
}
