/**
Authors: 
Dr. Claas Nendel <claas.nendel@zalf.de>
Xenia Specka <xenia.specka@zalf.de>
Michael Berg <michael.berg@zalf.de>

Maintainers: 
Currently maintained by the authors.

This file is part of the MONICA model. 
Copyright (C) 2007-2013, Leibniz Centre for Agricultural Landscape Research (ZALF)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef RUN_MONICA_H_
#define RUN_MONICA_H_

#include "../core/monica.h"
#include "production-process.h"

namespace Monica
{
  inline std::string pathSeparator()
  {
    return
    #ifdef __unix__
        "/";
#else
        "\\";
#endif
  }

  struct Env
  {
		enum
		{
			MODE_LC_DSS = 0,
			MODE_ACTIVATE_OUTPUT_FILES,
			MODE_HERMES,
			MODE_EVA2,
			MODE_SENSITIVITY_ANALYSIS,
			MODE_CC_GERMANY,
			MODE_MACSUR_SCALING,
			MODE_MACSUR_SCALING_CALIBRATION,
			MODE_CARBIOCIAL_CLUSTER
		};

    Env() {}

    /**
     * @brief Constructor
     * @param sps Soil parameters
     */
    Env(const Soil::SoilPMs* sps, CentralParameterProvider cpp);

    Env(Soil::SoilPMsPtr sps, CentralParameterProvider cpp);

    //Interface method for python wrapping. Simply returns number
    //of possible simulation steps according to avaible climate data.
    std::size_t numberOfPossibleSteps() const { return da.noOfStepsPossible(); }

    void addOrReplaceClimateData(std::string, const std::vector<double>& data);

  private:
    Soil::SoilPMsPtr _soilParamsPtr;
  public:
    //! a vector of soil parameter objects = layers of soil
    const Soil::SoilPMs* soilParams{nullptr};

    size_t noOfLayers{0};
    double layerThickness{0.0};

    bool useNMinMineralFertilisingMethod{false};
    MineralFertiliserParameters nMinFertiliserPartition;
    NMinUserParameters nMinUserParams;

    bool useAutomaticIrrigation{false};
    AutomaticIrrigationParameters autoIrrigationParams;
    MeasuredGroundwaterTableInformation groundwaterInformation;

    //! tell if farmer uses the secondary yield products
    bool useSecondaryYields{true};

    //static const int depthGroundwaterTable;   /**<  */
    double windSpeedHeight{0}; /**<  */
    double atmosphericCO2{-1}; /**< [ppm] */
    double albedo{0}; /**< Albedo [] */

    //! object holding the climate data
    Climate::DataAccessor da;

    //! vector of elements holding the data of the single crops in the rotation
    std::vector<ProductionProcess> cropRotation;

//    Tools::GridPoint gridPoint;        //! the gridpoint the model runs, just a convenience for the dss use
    int customId{-1};

    SiteParameters site;        //! site specific parameters
    GeneralParameters general;  //! general parameters to the model
    Soil::OrganicConstants organic;  //! constant organic parameters to the model

    CentralParameterProvider centralParameterProvider;

    std::string toString() const;
    std::string pathToOutputDir;

    //Set execution mode of Monica.
    //Disables debug outputs for some modes.
    void setMode(int m){ mode = m; }

    int getMode() const { return mode; }

    void setCropRotation(std::vector<ProductionProcess> cr) { cropRotation = cr; }

    std::string berestRequestAddress;
//    std::string berestRequestProtocol;
    //    std::string berestRequestHost;
//    std::string berestRequestPort;

    //zeromq reply socket for datastream controlled MONICA run
//    std::unique_ptr<zmq::socket_t> datastream;
    std::string inputDatastreamAddress;
    std::string inputDatastreamProtocol;
//    std::string inputDatastreamHost;
    std::string inputDatastreamPort;

    //zeromq publish socket for outputs of MONICA
//    std::unique_ptr<zmq::socket_t> outputstream;
    std::string outputDatastreamAddress;
    std::string outputDatastreamProtocol;
//    std::string outputDatastreamHost;
    std::string outputDatastreamPort;

  private:
    //! Variable for differentiate betweend execution modes of MONICA
    int mode{MODE_LC_DSS};
  };

  //------------------------------------------------------------------------------------------

	/*!
	* @brief structure holding all results of one monica run
	*/
	class Result
	{
	public:
		Result() {}
		~Result() {}

		//! just to keep track of the grid point the calculation is being made for
		Tools::GridPoint gp;
		//is as gp before used to track results when multiple parallel
		//unordered invocations of monica will happen
		int customId;

		//! vector of the result of one crop per year
		std::vector<PVResult> pvrs;

		//! results not regarding a particular crop in a rotation
		typedef std::map<ResultId, std::vector<double>> RId2Vector;
		RId2Vector generalResults;

		std::vector<double> getResultsById(int id);

		int sizeGeneralResults() { return int(generalResults.size()); }

		std::vector<std::string> dates;

		std::string toString();
	};

	//----------------------------------------------------------------------------

  std::pair<Tools::Date, std::map<Climate::ACD, double>>
  climateDataForStep(const Climate::DataAccessor& da, std::size_t stepNo);

  /*!
   * main function for running monica under a given Env(ironment)
   * @param env the environment completely defining what the model needs and gets
   * @return a structure with all the Monica results
   */
#ifndef	MONICA_GUI
  Result runMonica(Env env);
#else
  Result runMonica(Env env, Configuration* cfg = NULL);
#endif

	void initializeFoutHeader(std::ofstream&);
	void initializeGoutHeader(std::ofstream&);
	void writeCropResults(const CropGrowth*, std::ofstream&, std::ofstream&, bool);
	void writeGeneralResults(std::ofstream &fout, std::ofstream &gout, Env &env,
		MonicaModel &monica, int d);
	void dumpMonicaParametersIntoFile(std::string, CentralParameterProvider &cpp);

}

#endif