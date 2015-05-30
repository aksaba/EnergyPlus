// C++ Headers
#include <cmath>
#include <memory>

// ObjexxFCL Headers
#include <ObjexxFCL/Array.functions.hh>
#include <ObjexxFCL/Array3D.hh>
#include <ObjexxFCL/Fmath.hh>
#include <ObjexxFCL/gio.hh>

// EnergyPlus Headers
#include <PipeHeatTransfer.hh>
#include <BranchNodeConnections.hh>
#include <ConvectionCoefficients.hh>
#include <DataEnvironment.hh>
#include <DataHeatBalance.hh>
#include <DataHeatBalFanSys.hh>
#include <DataHVACGlobals.hh>
#include <DataIPShortCuts.hh>
#include <DataLoopNode.hh>
#include <DataPlant.hh>
#include <DataPrecisionGlobals.hh>
#include <FluidProperties.hh>
#include <General.hh>
#include <HeatBalanceInternalHeatGains.hh>
#include <InputProcessor.hh>
#include <NodeInputManager.hh>
#include <OutAirNodeManager.hh>
#include <OutputProcessor.hh>
#include <ScheduleManager.hh>
#include <UtilityRoutines.hh>

namespace EnergyPlus {

namespace PipeHeatTransfer {

	// Module containing the routines dealing with pipes with transport delay
	// and heat transfer.

	// MODULE INFORMATION:
	//       AUTHOR         Simon Rees
	//       DATE WRITTEN   July 2007
	//       MODIFIED       May 2008
	//       RE-ENGINEERED  na

	// PURPOSE OF THIS MODULE:
	// The purpose of this module is to simulate a pipe with heat transfer

	// METHODOLOGY EMPLOYED:
	// An implicit finite difference method is used to solve the temperature distribution of the
	// fluid in the pipe as a result of the transport delay and heat transfer to the environment.
	// For buried pipes, the simulation involves an implicit finite difference model of the soil,
	// which was originally based on Piechowski's thesis (below).  Equation numbers for
	// pipe:underground calculations are from Piechowski's thesis.  In Piechowski, the near-pipe
	// region is solved with a detailed finite difference grid, this current model makes use of
	// the Hanby model to simulate the actual pipe.

	// Kusuda, T. & Achenbach, P. (1965), "Earth temperature and thermal diffusivity at
	//     selected stations in the united states", ASHRAE Transactions 71(1), 61-75.
	// Piechowski, M. (1996), A Ground Coupled Heat Pump System with Energy Storage,
	//     PhD thesis, University of Melbourne.

	// OTHER NOTES: Equation Numbers listed in buried pipe routines are from Piechowski's thesis

	// Using/Aliasing
	using namespace DataPrecisionGlobals;
	using DataPlant::TypeOf_PipeExterior;
	using DataPlant::TypeOf_PipeInterior;
	using DataPlant::TypeOf_PipeUnderground;

	// MODULE PARAMETER DEFINITIONS
	static std::string const BlankString;

	int const None( 0 );
	int const ZoneEnv( 1 );
	int const ScheduleEnv( 2 );
	int const OutsideAirEnv( 3 );
	int const GroundEnv( 4 );

	int const PreviousTimeIndex( 1 );
	int const CurrentTimeIndex( 2 );
	int const TentativeTimeIndex( 3 );

	Real64 const InnerDeltaTime( 60.0 ); // one minute time step in seconds

	Array1D< std::shared_ptr< PipeHTData > > PipeHT;

	std::shared_ptr<PlantComponent>
	PipeHTData::pipeHTFactory( int objectType, std::string objectName ) {

		// SUBROUTINE INFORMATION:
		//       AUTHOR         Simon Rees
		//       DATE WRITTEN   July 2007
		//       MODIFIED       na
		//       RE-ENGINEERED  na
		// PURPOSE OF THIS SUBROUTINE:
		// This subroutine reads the input for hydronic Pipe Heat Transfers
		// from the user input file.  This will contain all of the information
		// needed to define and simulate the surface.
		
		using DataGlobals::NumOfZones;
		using DataGlobals::SecInHour;
		using DataGlobals::Pi;
		using DataHeatBalance::Construct;
		using DataHeatBalance::TotConstructs;
		using DataHeatBalance::Zone;
		using DataHeatBalance::Material;
		using DataHeatBalance::TotMaterials;
		using DataHeatBalance::IntGainTypeOf_PipeIndoor;
		using InputProcessor::FindItemInList;
		using InputProcessor::SameString;
		using InputProcessor::VerifyName;
		using namespace DataIPShortCuts; // Data for field names, blank numerics
		using NodeInputManager::GetOnlySingleNode;
		using BranchNodeConnections::TestCompSet;
		using General::RoundSigDigits;
		using namespace DataLoopNode;
		using ScheduleManager::GetScheduleIndex;
		using OutAirNodeManager::CheckOutAirNodeNumber;

		int const NumPipeSections( 20 );
		int const NumberOfDepthNodes( 8 ); // Number of nodes in the cartesian grid-Should be an even # for now
		Real64 const SecondsInHour( SecInHour );
		Real64 const HoursInDay( 24.0 );

		bool found = false;
		bool ErrorsFound( false ); // Set to true if errors in input,
		int IOStatus; // Used in GetObjectItem
		int NumAlphas; // Number of Alphas for each GetObjectItem call
		int NumNumbers; // Number of Numbers for each GetObjectItem call

		// create a new instance of a pipe heat transfer
		std::shared_ptr<PipeHTData> thisPipe( new PipeHTData() );

		switch ( objectType ) {
		case ( DataPlant::TypeOf_PipeExterior )
			cCurrentModuleObject = "Pipe:Outdoor";
			int const NumOfPipeHTExt = InputProcessor::GetNumObjectsFound( cCurrentModuleObject );
			for ( int PipeItem = 1; PipeItem <= NumOfPipeHTExt; ++PipeItem ) {

				InputProcessor::GetObjectItem( cCurrentModuleObject, PipeItem, cAlphaArgs, NumAlphas, rNumericArgs, NumNumbers, IOStatus, lNumericFieldBlanks, lAlphaFieldBlanks, cAlphaFieldNames, cNumericFieldNames );

				if ( objectName != cAlphaArgs( 1 ) ) {
					continue;
				}

				thisPipe->Name = cAlphaArgs( 1 );
				thisPipe->TypeOf = TypeOf_PipeExterior;

				// General user input data
				thisPipe->Construction = cAlphaArgs( 2 );
				thisPipe->ConstructionNum = FindItemInList( cAlphaArgs( 2 ), Construct.Name(), TotConstructs );

				if ( thisPipe->ConstructionNum == 0 ) {
					ShowSevereError( "Invalid " + cAlphaFieldNames( 2 ) + '=' + cAlphaArgs( 2 ) );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ErrorsFound = true;
				}

				//get inlet node data
				thisPipe->InletNode = cAlphaArgs( 3 );
				thisPipe->InletNodeNum = GetOnlySingleNode( cAlphaArgs( 3 ), ErrorsFound, cCurrentModuleObject, cAlphaArgs( 1 ), NodeType_Water, NodeConnectionType_Inlet, 1, ObjectIsNotParent );
				if ( thisPipe->InletNodeNum == 0 ) {
					ShowSevereError( "Invalid " + cAlphaFieldNames( 3 ) + '=' + cAlphaArgs( 3 ) );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ErrorsFound = true;
				}

				// get outlet node data
				thisPipe->OutletNode = cAlphaArgs( 4 );
				thisPipe->OutletNodeNum = GetOnlySingleNode( cAlphaArgs( 4 ), ErrorsFound, cCurrentModuleObject, cAlphaArgs( 1 ), NodeType_Water, NodeConnectionType_Outlet, 1, ObjectIsNotParent );
				if ( thisPipe->OutletNodeNum == 0 ) {
					ShowSevereError( "Invalid " + cAlphaFieldNames( 4 ) + '=' + cAlphaArgs( 4 ) );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ErrorsFound = true;
				}

				TestCompSet( cCurrentModuleObject, cAlphaArgs( 1 ), cAlphaArgs( 3 ), cAlphaArgs( 4 ), "Pipe Nodes" );

				// get environmental boundary condition type
				thisPipe->EnvironmentPtr = OutsideAirEnv;

				thisPipe->EnvrAirNode = cAlphaArgs( 5 );
				thisPipe->EnvrAirNodeNum = GetOnlySingleNode( cAlphaArgs( 5 ), ErrorsFound, cCurrentModuleObject, cAlphaArgs( 1 ), NodeType_Air, NodeConnectionType_OutsideAirReference, 1, ObjectIsNotParent );
				if ( ! lAlphaFieldBlanks( 5 ) ) {
					if ( ! CheckOutAirNodeNumber( thisPipe->EnvrAirNodeNum ) ) {
						ShowSevereError( "Invalid " + cAlphaFieldNames( 5 ) + '=' + cAlphaArgs( 5 ) );
						ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
						ShowContinueError( "Outdoor Air Node not on OutdoorAir:NodeList or OutdoorAir:Node" );
						ErrorsFound = true;
					}
				} else {
					ShowSevereError( "Invalid " + cAlphaFieldNames( 5 ) + '=' + cAlphaArgs( 5 ) );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ShowContinueError( "An " + cAlphaFieldNames( 5 ) + " must be used " );
					ErrorsFound = true;
				}

				// dimensions
				thisPipe->PipeID = rNumericArgs( 1 );
				if ( rNumericArgs( 1 ) <= 0.0 ) { // not really necessary because idd field has "minimum> 0"
					ShowSevereError( "Invalid " + cNumericFieldNames( 1 ) + " of " + RoundSigDigits( rNumericArgs( 1 ), 4 ) );
					ShowContinueError( cNumericFieldNames( 1 ) + " must be > 0.0" );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ErrorsFound = true;
				}

				thisPipe->Length = rNumericArgs( 2 );
				if ( rNumericArgs( 2 ) <= 0.0 ) { // not really necessary because idd field has "minimum> 0"
					ShowSevereError( "Invalid " + cNumericFieldNames( 2 ) + " of " + RoundSigDigits( rNumericArgs( 2 ), 4 ) );
					ShowContinueError( cNumericFieldNames( 2 ) + " must be > 0.0" );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ErrorsFound = true;
				}

				if ( thisPipe->ConstructionNum != 0 ) {
					ValidatePipeConstruction( cCurrentModuleObject, cAlphaArgs( 2 ), cAlphaFieldNames( 2 ), thisPipe->ConstructionNum, Item, ErrorsFound );
				}

			} // end of input loop
		case ( DataPlant::TypeOf_PipeInterior )
			cCurrentModuleObject = "Pipe:Indoor";
			int const NumOfPipeHTInt = InputProcessor::GetNumObjectsFound( cCurrentModuleObject );
			for ( int PipeItem = 1; PipeItem <= NumOfPipeHTInt; ++PipeItem ) {

				InputProcessor::GetObjectItem( cCurrentModuleObject, PipeItem, cAlphaArgs, NumAlphas, rNumericArgs, NumNumbers, IOStatus, lNumericFieldBlanks, lAlphaFieldBlanks, cAlphaFieldNames, cNumericFieldNames );

				if ( objectName != cAlphaArgs( 1 ) ) {
					continue;
				}

				thisPipe->Name = cAlphaArgs( 1 );
				thisPipe->TypeOf = TypeOf_PipeInterior;

				// General user input data
				thisPipe->Construction = cAlphaArgs( 2 );
				thisPipe->ConstructionNum = FindItemInList( cAlphaArgs( 2 ), Construct.Name(), TotConstructs );

				if ( thisPipe->ConstructionNum == 0 ) {
					ShowSevereError( "Invalid " + cAlphaFieldNames( 2 ) + '=' + cAlphaArgs( 2 ) );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ErrorsFound = true;
				}

				//get inlet node data
				thisPipe->InletNode = cAlphaArgs( 3 );
				thisPipe->InletNodeNum = GetOnlySingleNode( cAlphaArgs( 3 ), ErrorsFound, cCurrentModuleObject, cAlphaArgs( 1 ), NodeType_Water, NodeConnectionType_Inlet, 1, ObjectIsNotParent );
				if ( thisPipe->InletNodeNum == 0 ) {
					ShowSevereError( "Invalid " + cAlphaFieldNames( 3 ) + '=' + cAlphaArgs( 3 ) );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ErrorsFound = true;
				}

				// get outlet node data
				thisPipe->OutletNode = cAlphaArgs( 4 );
				thisPipe->OutletNodeNum = GetOnlySingleNode( cAlphaArgs( 4 ), ErrorsFound, cCurrentModuleObject, cAlphaArgs( 1 ), NodeType_Water, NodeConnectionType_Outlet, 1, ObjectIsNotParent );
				if ( thisPipe->OutletNodeNum == 0 ) {
					ShowSevereError( "Invalid " + cAlphaFieldNames( 4 ) + '=' + cAlphaArgs( 4 ) );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ErrorsFound = true;
				}

				TestCompSet( cCurrentModuleObject, cAlphaArgs( 1 ), cAlphaArgs( 3 ), cAlphaArgs( 4 ), "Pipe Nodes" );

				// get environmental boundary condition type

				if ( lAlphaFieldBlanks( 5 ) ) cAlphaArgs( 5 ) = "ZONE";

				{ auto const SELECT_CASE_var( cAlphaArgs( 5 ) );

				if ( SELECT_CASE_var == "ZONE" ) {
					thisPipe->EnvironmentPtr = ZoneEnv;
					thisPipe->EnvrZone = cAlphaArgs( 6 );
					thisPipe->EnvrZonePtr = FindItemInList( cAlphaArgs( 6 ), Zone.Name(), NumOfZones );
					if ( thisPipe->EnvrZonePtr == 0 ) {
						ShowSevereError( "Invalid " + cAlphaFieldNames( 6 ) + '=' + cAlphaArgs( 6 ) );
						ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
						ErrorsFound = true;
					}

				} else if ( SELECT_CASE_var == "SCHEDULE" ) {
					thisPipe->EnvironmentPtr = ScheduleEnv;
					thisPipe->EnvrSchedule = cAlphaArgs( 7 );
					thisPipe->EnvrSchedPtr = GetScheduleIndex( thisPipe->EnvrSchedule );
					thisPipe->EnvrVelSchedule = cAlphaArgs( 8 );
					thisPipe->EnvrVelSchedPtr = GetScheduleIndex( thisPipe->EnvrVelSchedule );
					if ( thisPipe->EnvrSchedPtr == 0 ) {
						ShowSevereError( "Invalid " + cAlphaFieldNames( 7 ) + '=' + cAlphaArgs( 7 ) );
						ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
						ErrorsFound = true;
					}
					if ( thisPipe->EnvrVelSchedPtr == 0 ) {
						ShowSevereError( "Invalid " + cAlphaFieldNames( 8 ) + '=' + cAlphaArgs( 8 ) );
						ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
						ErrorsFound = true;
					}

				} else {
					ShowSevereError( "Invalid " + cAlphaFieldNames( 5 ) + '=' + cAlphaArgs( 5 ) );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ShowContinueError( "Should be \"ZONE\" or \"SCHEDULE\"" ); //TODO rename point
					ErrorsFound = true;

				}}

				// dimensions
				thisPipe->PipeID = rNumericArgs( 1 );
				if ( rNumericArgs( 1 ) <= 0.0 ) { // not really necessary because idd field has "minimum> 0"
					ShowSevereError( "GetPipesHeatTransfer: invalid " + cNumericFieldNames( 1 ) + " of " + RoundSigDigits( rNumericArgs( 1 ), 4 ) );
					ShowContinueError( cNumericFieldNames( 1 ) + " must be > 0.0" );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ErrorsFound = true;
				}

				thisPipe->Length = rNumericArgs( 2 );
				if ( rNumericArgs( 2 ) <= 0.0 ) { // not really necessary because idd field has "minimum> 0"
					ShowSevereError( "GetPipesHeatTransfer: invalid " + cNumericFieldNames( 2 ) + " of " + RoundSigDigits( rNumericArgs( 2 ), 4 ) );
					ShowContinueError( cNumericFieldNames( 2 ) + " must be > 0.0" );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ErrorsFound = true;
				}

				if ( thisPipe->ConstructionNum != 0 ) {
					ValidatePipeConstruction( cCurrentModuleObject, cAlphaArgs( 2 ), cAlphaFieldNames( 2 ), thisPipe->ConstructionNum, Item, ErrorsFound );
				}
			}
		case ( DataPlant::TypeOf_PipeUnderground )
			cCurrentModuleObject = "Pipe:Underground";
			int const NumOfPipeHTUG = InputProcessor::GetNumObjectsFound( cCurrentModuleObject );
			for ( int PipeItem = 1; PipeItem <= NumOfPipeHTUG; ++PipeItem ) {

				InputProcessor::GetObjectItem( cCurrentModuleObject, PipeItem, cAlphaArgs, NumAlphas, rNumericArgs, NumNumbers, IOStatus, lNumericFieldBlanks, lAlphaFieldBlanks, cAlphaFieldNames, cNumericFieldNames );

				if ( objectName != cAlphaArgs( 1 ) ) {
					continue;
				}
				
				thisPipe->Name = cAlphaArgs( 1 );
				thisPipe->TypeOf = TypeOf_PipeUnderground;

				// General user input data
				thisPipe->Construction = cAlphaArgs( 2 );
				thisPipe->ConstructionNum = FindItemInList( cAlphaArgs( 2 ), Construct.Name(), TotConstructs );

				if ( thisPipe->ConstructionNum == 0 ) {
					ShowSevereError( "Invalid " + cAlphaFieldNames( 2 ) + '=' + cAlphaArgs( 2 ) );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ErrorsFound = true;
				}

				//get inlet node data
				thisPipe->InletNode = cAlphaArgs( 3 );
				thisPipe->InletNodeNum = GetOnlySingleNode( cAlphaArgs( 3 ), ErrorsFound, cCurrentModuleObject, cAlphaArgs( 1 ), NodeType_Water, NodeConnectionType_Inlet, 1, ObjectIsNotParent );
				if ( thisPipe->InletNodeNum == 0 ) {
					ShowSevereError( "Invalid " + cAlphaFieldNames( 3 ) + '=' + cAlphaArgs( 3 ) );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ErrorsFound = true;
				}

				// get outlet node data
				thisPipe->OutletNode = cAlphaArgs( 4 );
				thisPipe->OutletNodeNum = GetOnlySingleNode( cAlphaArgs( 4 ), ErrorsFound, cCurrentModuleObject, cAlphaArgs( 1 ), NodeType_Water, NodeConnectionType_Outlet, 1, ObjectIsNotParent );
				if ( thisPipe->OutletNodeNum == 0 ) {
					ShowSevereError( "Invalid " + cAlphaFieldNames( 4 ) + '=' + cAlphaArgs( 4 ) );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ErrorsFound = true;
				}

				TestCompSet( cCurrentModuleObject, cAlphaArgs( 1 ), cAlphaArgs( 3 ), cAlphaArgs( 4 ), "Pipe Nodes" );

				thisPipe->EnvironmentPtr = GroundEnv;

				// Solar inclusion flag
				// A6,  \field Sun Exposure
				if ( SameString( cAlphaArgs( 5 ), "SUNEXPOSED" ) ) {
					thisPipe->SolarExposed = true;
				} else if ( SameString( cAlphaArgs( 5 ), "NOSUN" ) ) {
					thisPipe->SolarExposed = false;
				} else {
					ShowSevereError( "GetPipesHeatTransfer: invalid key for sun exposure flag for " + cAlphaArgs( 1 ) );
					ShowContinueError( "Key should be either SunExposed or NoSun.  Entered Key: " + cAlphaArgs( 5 ) );
					ErrorsFound = true;
				}

				// dimensions
				thisPipe->PipeID = rNumericArgs( 1 );
				if ( rNumericArgs( 1 ) <= 0.0 ) { // not really necessary because idd field has "minimum> 0"
					ShowSevereError( "Invalid " + cNumericFieldNames( 1 ) + " of " + RoundSigDigits( rNumericArgs( 1 ), 4 ) );
					ShowContinueError( cNumericFieldNames( 1 ) + " must be > 0.0" );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ErrorsFound = true;
				}

				thisPipe->Length = rNumericArgs( 2 );
				if ( rNumericArgs( 2 ) <= 0.0 ) { // not really necessary because idd field has "minimum> 0"
					ShowSevereError( "Invalid " + cNumericFieldNames( 2 ) + " of " + RoundSigDigits( rNumericArgs( 2 ), 4 ) );
					ShowContinueError( cNumericFieldNames( 2 ) + " must be > 0.0" );
					ShowContinueError( "Entered in " + cCurrentModuleObject + '=' + cAlphaArgs( 1 ) );
					ErrorsFound = true;
				}

				// Also get the soil material name
				// A7,  \field Soil Material
				thisPipe->SoilMaterial = cAlphaArgs( 6 );
				thisPipe->SoilMaterialNum = FindItemInList( cAlphaArgs( 6 ), Material.Name(), TotMaterials );
				if ( thisPipe->SoilMaterialNum == 0 ) {
					ShowSevereError( "Invalid " + cAlphaFieldNames( 6 ) + '=' + thisPipe->SoilMaterial );
					ShowContinueError( "Found in " + cCurrentModuleObject + '=' + thisPipe->Name );
					ErrorsFound = true;
				} else {
					thisPipe->SoilDensity = Material( thisPipe->SoilMaterialNum ).Density;
					thisPipe->SoilDepth = Material( thisPipe->SoilMaterialNum ).Thickness;
					thisPipe->SoilCp = Material( thisPipe->SoilMaterialNum ).SpecHeat;
					thisPipe->SoilConductivity = Material( thisPipe->SoilMaterialNum ).Conductivity;
					thisPipe->SoilThermAbs = Material( thisPipe->SoilMaterialNum ).AbsorpThermal;
					thisPipe->SoilSolarAbs = Material( thisPipe->SoilMaterialNum ).AbsorpSolar;
					thisPipe->SoilRoughness = Material( thisPipe->SoilMaterialNum ).Roughness;
					thisPipe->PipeDepth = thisPipe->SoilDepth + thisPipe->PipeID / 2.0;
					thisPipe->DomainDepth = thisPipe->PipeDepth * 2.0;
					thisPipe->SoilDiffusivity = thisPipe->SoilConductivity / ( thisPipe->SoilDensity * thisPipe->SoilCp );
					thisPipe->SoilDiffusivityPerDay = thisPipe->SoilDiffusivity * SecondsInHour * HoursInDay;

					// Mesh the cartesian domain
					thisPipe->NumDepthNodes = NumberOfDepthNodes;
					thisPipe->PipeNodeDepth = thisPipe->NumDepthNodes / 2;
					thisPipe->PipeNodeWidth = thisPipe->NumDepthNodes / 2;
					thisPipe->DomainDepth = thisPipe->PipeDepth * 2.0;
					thisPipe->dSregular = thisPipe->DomainDepth / ( thisPipe->NumDepthNodes - 1 );
				}

				// Now we need to see if average annual temperature data is brought in here
				if ( NumNumbers >= 3 ) {
					thisPipe->AvgAnnualManualInput = 1;

					//If so, we need to read in the data
					// N3,  \field Average soil surface temperature
					thisPipe->AvgGroundTemp = rNumericArgs( 3 );

					// N4,  \field Amplitude of soil surface temperature
					if ( NumNumbers >= 4 ) {
						thisPipe->AvgGndTempAmp = rNumericArgs( 4 );
						if ( thisPipe->AvgGndTempAmp < 0.0 ) {
							ShowSevereError( "Invalid " + cNumericFieldNames( 4 ) + '=' + RoundSigDigits( thisPipe->AvgGndTempAmp, 2 ) );
							ShowContinueError( "Found in " + cCurrentModuleObject + '=' + thisPipe->Name );
							ErrorsFound = true;
						}
					}

					// N5;  \field Phase constant of soil surface temperature
					if ( NumNumbers >= 5 ) {
						thisPipe->PhaseShiftDays = rNumericArgs( 5 );
						if ( thisPipe->PhaseShiftDays < 0 ) {
							ShowSevereError( "Invalid " + cNumericFieldNames( 5 ) + '=' + RoundSigDigits( thisPipe->PhaseShiftDays ) );
							ShowContinueError( "Found in " + cCurrentModuleObject + '=' + thisPipe->Name );
							ErrorsFound = true;
						}
					}

					if ( NumNumbers >= 3 && NumNumbers < 5 ) {
						ShowSevereError( cCurrentModuleObject + '=' + thisPipe->Name );
						ShowContinueError( "If any one annual ground temperature item is entered, all 3 items must be entered" );
						ErrorsFound = true;
					}

				}

				if ( thisPipe->ConstructionNum != 0 ) {
					ValidatePipeConstruction( cCurrentModuleObject, cAlphaArgs( 2 ), cAlphaFieldNames( 2 ), thisPipe->ConstructionNum, Item, ErrorsFound );
				}

				// Select number of pipe sections.  Hanby's optimal number of 20 section is selected.
				NumSections = NumPipeSections;
				thisPipe->NumSections = NumPipeSections;

				// For buried pipes, we need to allocate the cartesian finite difference array
				thisPipe->T.allocate( thisPipe->PipeNodeWidth, thisPipe->NumDepthNodes, thisPipe->NumSections, TentativeTimeIndex );
				thisPipe->T = 0.0;

			} // PipeUG input loop
		}
		NumSections = NumPipeSections;
		thisPipe->NumSections = NumPipeSections;

		// We need to allocate the Hanby model arrays for all pipes, including buried
		thisPipe->TentativeFluidTemp.allocate( {0,NumSections}, 0.0 );
		thisPipe->TentativePipeTemp.allocate( {0,NumSections}, 0.0 );
		thisPipe->FluidTemp.allocate( {0,NumSections}, 0.0 );
		thisPipe->PreviousFluidTemp.allocate( {0,NumSections}, 0.0 );
		thisPipe->PipeTemp.allocate( {0,NumSections}, 0.0 );
		thisPipe->PreviousPipeTemp.allocate( {0,NumSections}, 0.0 );

		// work out heat transfer areas (area per section)
		thisPipe->InsideArea = Pi * thisPipe->PipeID * thisPipe->Length / NumSections;
		thisPipe->OutsideArea = Pi * ( thisPipe->PipeOD + 2 * thisPipe->InsulationThickness ) * thisPipe->Length / NumSections;

		// cross sectional area
		thisPipe->SectionArea = Pi * 0.25 * pow_2( thisPipe->PipeID );

		// pipe & insulation mass
		thisPipe->PipeHeatCapacity = thisPipe->PipeCp * thisPipe->PipeDensity * ( Pi * 0.25 * pow_2( thisPipe->PipeOD ) - thisPipe->SectionArea ); // the metal component

		// final error check
		if ( ErrorsFound ) {
			ShowFatalError( "GetPipesHeatTransfer: Errors found in input. Preceding conditions cause termination." );
		}

		// Set up the output variables CurrentModuleObject='Pipe:Indoor/Outdoor/Underground'
		SetupOutputVariable( "Pipe Fluid Heat Transfer Rate [W]", thisPipe->FluidHeatLossRate, "Plant", "Average", thisPipe->Name );
		SetupOutputVariable( "Pipe Fluid Heat Transfer Energy [J]", thisPipe->FluidHeatLossEnergy, "Plant", "Sum", thisPipe->Name );

		if ( thisPipe->EnvironmentPtr == ZoneEnv ) {
			SetupOutputVariable( "Pipe Ambient Heat Transfer Rate [W]", thisPipe->EnvironmentHeatLossRate, "Plant", "Average", thisPipe->Name );
			SetupOutputVariable( "Pipe Ambient Heat Transfer Energy [J]", thisPipe->EnvHeatLossEnergy, "Plant", "Sum", thisPipe->Name );
			SetupZoneInternalGain( thisPipe->EnvrZonePtr, "Pipe:Indoor", thisPipe->Name, IntGainTypeOf_PipeIndoor, thisPipe->ZoneHeatGainRate );
		}

		SetupOutputVariable( "Pipe Mass Flow Rate [kg/s]", thisPipe->MassFlowRate, "Plant", "Average", thisPipe->Name );
		SetupOutputVariable( "Pipe Volume Flow Rate [m3/s]", thisPipe->VolumeFlowRate, "Plant", "Average", thisPipe->Name );
		SetupOutputVariable( "Pipe Inlet Temperature [C]", thisPipe->FluidInletTemp, "Plant", "Average", thisPipe->Name );
		SetupOutputVariable( "Pipe Outlet Temperature [C]", thisPipe->FluidOutletTemp, "Plant", "Average", thisPipe->Name );


		if ( found && !ErrorsFound ) {
			PipeHT.push_back( thisPipe );
			return thisPipe;
		} else {
			ShowFatalError( "GetPipeHTInput: Errors getting input for pipes" );
			// add a dummy return here to hush up the compiler
			return nullptr;
		}
	}

	int
	PipeHTData::performEveryTimeInit() {

		// Assign variable
		CurSimDay = double( DayOfSim );

		// some useful module variables
		InletNodeNum = this->InletNodeNum;
		OutletNodeNum = this->OutletNodeNum;
		MassFlowRate = Node( InletNodeNum ).MassFlowRate;
		InletTemp = Node( InletNodeNum ).Temp;

		// time step in seconds
		DeltaTime = TimeStepSys * SecInHour;
		NumInnerTimeSteps = int( DeltaTime / InnerDeltaTime );

		//Calculate the current sim time for this pipe (not necessarily structure variable, but it is ok for consistency)
		this->CurrentSimTime = ( DayOfSim - 1 ) * 24 + HourOfDay - 1 + ( TimeStep - 1 ) * TimeStepZone + SysTimeElapsed;
		if ( std::abs( this->CurrentSimTime - this->PreviousSimTime ) > 1.0e-6 ) {
			PushArrays = true;
			this->PreviousSimTime = this->CurrentSimTime;
		} else {
			PushArrays = false; //Time hasn't passed, don't accept the tentative values yet!
		}

		if ( PushArrays ) {

			//If sim time has changed all values from previous runs should have been acceptable.
			// Thus we will now shift the arrays from 2>1 and 3>2 so we can then begin
			// to update 2 and 3 again.
			if ( this->EnvironmentPtr == GroundEnv ) {
				for ( LengthIndex = 2; LengthIndex <= this->NumSections; ++LengthIndex ) {
					for ( DepthIndex = 1; DepthIndex <= this->NumDepthNodes; ++DepthIndex ) {
						for ( WidthIndex = 2; WidthIndex <= this->PipeNodeWidth; ++WidthIndex ) {
							//This will essentially 'accept' the tentative values that were calculated last iteration
							// as the new officially 'current' values
							this->T( WidthIndex, DepthIndex, LengthIndex, CurrentTimeIndex ) = this->T( WidthIndex, DepthIndex, LengthIndex, TentativeTimeIndex );
						}
					}
				}
			}

			//Then update the Hanby near pipe model temperatures
			this->FluidTemp = this->TentativeFluidTemp;
			this->PipeTemp = this->TentativePipeTemp;

		} else {

			//If we don't have FirstHVAC, the last iteration values were not accepted, and we should
			// not step through time.  Thus we will revert our T(3,:,:,:) array back to T(2,:,:,:) to
			// start over with the same values as last time.
			for ( LengthIndex = 2; LengthIndex <= this->NumSections; ++LengthIndex ) {
				for ( DepthIndex = 1; DepthIndex <= this->NumDepthNodes; ++DepthIndex ) {
					for ( WidthIndex = 2; WidthIndex <= this->PipeNodeWidth; ++WidthIndex ) {
						//This will essentially erase the past iterations and revert back to the correct values
						this->T( WidthIndex, DepthIndex, LengthIndex, TentativeTimeIndex ) = this->T( WidthIndex, DepthIndex, LengthIndex, CurrentTimeIndex );
					}
				}
			}

			//Similarly for Hanby model arrays
			this->TentativeFluidTemp = this->FluidTemp;
			this->TentativePipeTemp = this->PipeTemp;

		}

		//This still catches even in winter design day
		//Even though the loop eventually has no flow rate, it appears it initializes to a value, then converges to OFF
		//Thus, this is called at the beginning of every time step once.

		this->FluidSpecHeat = GetSpecificHeatGlycol( PlantLoop( this->LoopNum ).FluidName, InletTemp, PlantLoop( this->LoopNum ).FluidIndex, RoutineName );
		this->FluidDensity = GetDensityGlycol( PlantLoop( this->LoopNum ).FluidName, InletTemp, PlantLoop( this->LoopNum ).FluidIndex, RoutineName );

		// At this point, for all Pipe:Interior objects we should zero out the energy and rate arrays
		this->FluidHeatLossRate = 0.0;
		this->FluidHeatLossEnergy = 0.0;
		this->EnvironmentHeatLossRate = 0.0;
		this->EnvHeatLossEnergy = 0.0;
		this->ZoneHeatGainRate = 0.0;
		this->FluidHeatLossRate = 0.0;
		this->EnvHeatLossRate = 0.0;
		this->OutletTemp = 0.0;

		if ( this->FluidDensity > 0.0 ) {
			//The density will only be zero the first time through, which will be a warmup day, and not reported
			this->VolumeFlowRate = this->MassFlowRate / this->FluidDensity;
		}

	}

	int
	PipeHTData::performOneTimeInit() {

		int const MonthsInYear( 12 ); // Number of months in the year
		int const AvgDaysInMonth( 30 ); // Average days in a month
		Real64 const LargeNumber( 9999.9 ); // Large number (compared to temperature values)

		errFlag = false;
		ScanPlantLoopsForObject( this->Name, this->TypeOf, this->LoopNum, this->LoopSideNum, this->BranchNum, this->CompNum, _, _, _, _, _, errFlag );

		//If there are any underground buried pipes, we must bring in data
		if ( this->EnvironmentPtr == GroundEnv ) {

			//If ground temp data was not brought in manually in GETINPUT,
			// then we must get it from the surface ground temperatures
			if ( this->AvgAnnualManualInput == 0 ) {

				if ( ! PubGroundTempSurfFlag ) {
					ShowFatalError( "No Site:GroundTemperature:Shallow object found.  This is required for a Pipe:Underground object." );
				}

				//Calculate Average Ground Temperature for all 12 months of the year:
				this->AvgGroundTemp = 0.0;
				for ( MonthIndex = 1; MonthIndex <= MonthsInYear; ++MonthIndex ) {
					this->AvgGroundTemp += PubGroundTempSurface( MonthIndex );
				}
				this->AvgGroundTemp /= MonthsInYear;

				//Calculate Average Amplitude from Average:
				this->AvgGndTempAmp = 0.0;
				for ( MonthIndex = 1; MonthIndex <= MonthsInYear; ++MonthIndex ) {
					this->AvgGndTempAmp += std::abs( PubGroundTempSurface( MonthIndex ) - this->AvgGroundTemp );
				}
				this->AvgGndTempAmp /= MonthsInYear;

				//Also need to get the month of minimum surface temperature to set phase shift for Kusuda and Achenbach:
				this->MonthOfMinSurfTemp = 0;
				this->MinSurfTemp = LargeNumber; //Set high so that the first months temp will be lower and actually get updated
				for ( MonthIndex = 1; MonthIndex <= MonthsInYear; ++MonthIndex ) {
					if ( PubGroundTempSurface( MonthIndex ) <= this->MinSurfTemp ) {
						this->MonthOfMinSurfTemp = MonthIndex;
						this->MinSurfTemp = PubGroundTempSurface( MonthIndex );
					}
				}
				this->PhaseShiftDays = this->MonthOfMinSurfTemp * AvgDaysInMonth;
				
			} //End manual ground data input structure
		}
		if ( errFlag ) {
			ShowFatalError( "InitPipesHeatTransfer: Program terminated due to previous condition(s)." );
		}
	}

	int
	PipeHTData::performBeginEnvrnInit() {
		
		// For underground pipes, we need to re-init the cartesian array each environment
		if ( this->EnvironmentPtr == GroundEnv ) {
			for ( int TimeIndex = PreviousTimeIndex; TimeIndex <= TentativeTimeIndex; ++TimeIndex ) {
				//Loop through all length, depth, and width of pipe to init soil temperature
				for ( int LengthIndex = 1; LengthIndex <= PipeHT( PipeNum ).NumSections; ++LengthIndex ) {
					for ( int DepthIndex = 1; DepthIndex <= PipeHT( PipeNum ).NumDepthNodes; ++DepthIndex ) {
						for ( int WidthIndex = 1; WidthIndex <= PipeHT( PipeNum ).PipeNodeWidth; ++WidthIndex ) {
							Real64 CurrentDepth = ( DepthIndex - 1 ) * PipeHT( PipeNum ).dSregular;
							this->T( WidthIndex, DepthIndex, LengthIndex, TimeIndex ) = TBND( CurrentDepth, CurSimDay, PipeNum );
						}
					}
				}
			}
		}

		// We also need to re-init the Hanby arrays for all pipes, including buried
		Real64 const FirstTemperatures = 21.0;
		this->TentativeFluidTemp = FirstTemperatures;
		this->FluidTemp = FirstTemperatures;
		this->PreviousFluidTemp = FirstTemperatures;
		this->TentativePipeTemp = FirstTemperatures;
		this->PipeTemp = FirstTemperatures;
		this->PreviousPipeTemp = FirstTemperatures;
		this->PreviousSimTime = 0.0;
		this->DeltaTime = 0.0;
		this->OutletTemp = 0.0;
		this->EnvironmentTemp = 0.0;
		this->EnvHeatLossRate = 0.0;
		this->FluidHeatLossRate = 0.0;
		this->ZoneHeatGainRate = 0.0;
	}

	int
	PipeHTData::performFirstHVACInit() {
	
		//We need to update boundary conditions here, as well as updating the arrays
		if ( this->EnvironmentPtr == GroundEnv ) {

			// And then update Ground Boundary Conditions
			for ( int TimeIndex = 1; TimeIndex <= TentativeTimeIndex; ++TimeIndex ) {
				for ( int LengthIndex = 1; LengthIndex <= this->NumSections; ++LengthIndex ) {
					for ( int DepthIndex = 1; DepthIndex <= this->NumDepthNodes; ++DepthIndex ) {
						//Farfield boundary
						Real64 CurrentDepth = ( DepthIndex - 1 ) * this->dSregular;
						this->T( 1, DepthIndex, LengthIndex, TimeIndex ) = TBND( CurrentDepth, CurSimDay, PipeHTNum );
					}
					for ( WidthIndex = 1; WidthIndex <= this->PipeNodeWidth; ++WidthIndex ) {
						//Bottom side of boundary
						Real64 CurrentDepth = this->DomainDepth;
						this->T( WidthIndex, this->NumDepthNodes, LengthIndex, TimeIndex ) = CurTemp = TBND( CurrentDepth, CurSimDay, PipeHTNum );
					}
				}
			}
		}

		// should next choose environment temperature according to coupled with air or ground
		{ auto const SELECT_CASE_var( this->EnvironmentPtr );
		if ( SELECT_CASE_var == GroundEnv ) {
			//EnvironmentTemp = GroundTemp
		} else if ( SELECT_CASE_var == OutsideAirEnv ) {
			EnvironmentTemp = OutDryBulbTemp;
		} else if ( SELECT_CASE_var == ZoneEnv ) {
			EnvironmentTemp = MAT( this->EnvrZonePtr );
		} else if ( SELECT_CASE_var == ScheduleEnv ) {
			EnvironmentTemp = GetCurrentScheduleValue( this->EnvrSchedPtr );
		} else if ( SELECT_CASE_var == None ) { //default to outside temp
			EnvironmentTemp = OutDryBulbTemp;
		}}
	}

	int
	PipeHTData::simulate() {
		// make the calculations
		for ( int InnerTimeStepCtr = 1; InnerTimeStepCtr <= NumInnerTimeSteps; ++InnerTimeStepCtr ) {
			{ auto const SELECT_CASE_var( this->EnvironmentPtr );
			if ( SELECT_CASE_var == GroundEnv ) {
				CalcBuriedPipeSoil( PipeHTNum );
			} else {
				CalcPipesHeatTransfer( PipeHTNum );
			}}
			PushInnerTimeStepArrays( PipeHTNum );
		}
		// update vaiables
		UpdatePipesHeatTransfer();
		// update report variables
		ReportPipesHeatTransfer( PipeHTNum );
	}

	void
	PipeHTData::pushInnerTimeStepArrays() {
		if ( this->EnvironmentPtr == GroundEnv ) {
			for ( int LengthIndex = 2; LengthIndex <= this->NumSections; ++LengthIndex ) {
				for ( int DepthIndex = 1; DepthIndex <= this->NumDepthNodes; ++DepthIndex ) {
					for ( int WidthIndex = 2; WidthIndex <= this->PipeNodeWidth; ++WidthIndex ) {
						//This will store the old 'current' values as the new 'previous values'  This allows
						// us to use the previous time array as history terms in the equations
						this->T( WidthIndex, DepthIndex, LengthIndex, PreviousTimeIndex ) = this->T( WidthIndex, DepthIndex, LengthIndex, CurrentTimeIndex );
					}
				}
			}
		}
		//Then update the Hanby near pipe model temperatures
		this->PreviousFluidTemp = this->FluidTemp;
		this->PreviousPipeTemp = this->PipeTemp;
	}

	int
	PipeHTData::validatePipeConstruction() {

		// SUBROUTINE INFORMATION:
		//       AUTHOR         Linda Lawrie
		//       DATE WRITTEN   August 2008
		//       MODIFIED       na
		//       RE-ENGINEERED  na

		// PURPOSE OF THIS SUBROUTINE:
		// This routine, called from GetInput, validates the pipe construction usage.

		// SUBROUTINE LOCAL VARIABLE DECLARATIONS:
		Real64 Resistance = 0; // overall thermal resistance [m^2.C/W]
		Real64 Density; // average density [kg/m^3]
		Real64 TotThickness = 0; // total thickness of all layers
		Real64 SpHeat; // average specific heat [J/kg.K]
		int LayerNum;
		int TotalLayers; // total number of layers (pipe layer + insulation layers)

		// CTF stuff
		TotalLayers = DataHeatBalance::Construct( ConstructionNum ).TotLayers;
		auto & thisConstruct = DataHeatBalance::Construct( this->ConstructionNum );
		
		// get pipe properties
		if ( TotalLayers == 1 ) { // no insulation layer

			auto & firstLayer = DataHeatBalance::Material( thisConstruct.LayerPoint( 1 ) );
			this->PipeConductivity = firstLayer.Conductivity;
			this->PipeDensity = firstLayer.Density;
			this->PipeCp = firstLayer.SpecHeat;
			this->PipeOD = this->PipeID + 2.0 * firstLayer.Thickness;
			this->InsulationOD = this->PipeOD;
			this->SumTK = firstLayer.Thickness / firstLayer.Conductivity;

		} else if ( TotalLayers >= 2 ) { // first layers are insulation, last layer is pipe

			for ( LayerNum = 1; LayerNum <= TotalLayers - 1; ++LayerNum ) {
				auto & thisLayer = DataHeatBalance::Material( thisConstruct.LayerPoint( LayerNum ) );
				Resistance += thisLayer.Thickness / thisLayer.Conductivity;
				Density = thisLayer.Density * thisLayer.Thickness;
				TotThickness += thisLayer.Thickness;
				SpHeat = thisLayer.SpecHeat * thisLayer.Thickness;
				this->InsulationThickness = thisLayer.Thickness;
				this->SumTK += thisLayer.Thickness / thisLayer.Conductivity;
			}

			this->InsulationResistance = Resistance;
			this->InsulationConductivity = TotThickness / Resistance;
			this->InsulationDensity = Density / TotThickness;
			this->InsulationCp = SpHeat / TotThickness;
			this->InsulationThickness = TotThickness;

			auto & lastLayer = DataHeatBalance::Material( thisConstruct.LayerPoint( LayerNum ) );
			this->PipeConductivity = lastLayer.Conductivity;
			this->PipeDensity = lastLayer.Density;
			this->PipeCp = lastLayer.SpecHeat;
			this->PipeOD = this->PipeID + 2.0 * lastLayer.Thickness;
			this->InsulationOD = this->PipeOD + 2.0 * this->InsulationThickness;

		}
		return 0;
	}

	void
	PipeHTData::calcPipesHeatTransfer() {

		//       AUTHOR         Simon Rees
		//       DATE WRITTEN   July 2007
		//       MODIFIED       na
		//       RE-ENGINEERED  na

		// PURPOSE OF THIS SUBROUTINE:
		// This subroutine does all of the stuff that is necessary to simulate
		// a Pipe Heat Transfer.  Calls are made to appropriate routines
		// for heat transfer coefficients

		// METHODOLOGY EMPLOYED:
		// Differential equations for pipe and fluid nodes along the pipe are solved
		// taking backward differences in time.
		// The heat loss/gain calculations are run continuously, even when the loop is off.
		// Fluid temps will drift according to environmental conditions when there is zero flow.

		// REFERENCES:

		// Using/Aliasing
		using namespace DataEnvironment;

		// Locals
		// SUBROUTINE ARGUMENT DEFINITIONS:

		// SUBROUTINE PARAMETER DEFINITIONS:

		// INTERFACE BLOCK SPECIFICATIONS
		// na

		// DERIVED TYPE DEFINITIONS
		// na

		// SUBROUTINE LOCAL VARIABLE DECLARATIONS:

		// fluid node heat balance (see engineering doc).
		static Real64 A1( 0.0 ); // sum of the heat balance terms
		static Real64 A2( 0.0 ); // mass flow term
		static Real64 A3( 0.0 ); // inside pipe wall convection term
		static Real64 A4( 0.0 ); // fluid node heat capacity term
		// pipe wall node heat balance (see engineering doc).
		static Real64 B1( 0.0 ); // sum of the heat balance terms
		static Real64 B2( 0.0 ); // inside pipe wall convection term
		static Real64 B3( 0.0 ); // outside pipe wall convection term
		static Real64 B4( 0.0 ); // fluid node heat capacity term

		static Real64 AirConvCoef( 0.0 ); // air-pipe convection coefficient
		static Real64 FluidConvCoef( 0.0 ); // fluid-pipe convection coefficient
		static Real64 EnvHeatTransCoef( 0.0 ); // external convection coefficient (outside pipe)
		static Real64 FluidNodeHeatCapacity( 0.0 ); // local var for MCp for single node of pipe

		static int PipeDepth( 0 );
		static int PipeWidth( 0 );
		int curnode;
		Real64 TempBelow;
		Real64 TempBeside;
		Real64 TempAbove;
		Real64 Numerator;
		Real64 Denominator;
		Real64 SurfaceTemp;

		// traps fluid properties problems such as freezing conditions
		if ( this->FluidSpecHeat <= 0.0 || this->FluidDensity <= 0.0 ) {
			// leave the state of the pipe as it was
			OutletTemp = this->TentativeFluidTemp( this->NumSections );
			// set heat transfer rates to zero for consistency
			EnvHeatLossRate = 0.0;
			FluidHeatLossRate = 0.0;
			return;
		}

		//  AirConvCoef =  OutsidePipeHeatTransCoef(PipeHTNum)
		// Revised by L. Gu by including insulation conductance 6/19/08

		if ( this->EnvironmentPtr != GroundEnv ) {
			AirConvCoef = 1.0 / ( 1.0 / OutsidePipeHeatTransCoef( PipeHTNum ) + this->InsulationResistance );
		}

		FluidConvCoef = CalcPipeHeatTransCoef( PipeHTNum, InletTemp, MassFlowRate, this->PipeID );

		// heat transfer to air or ground
		{ auto const SELECT_CASE_var( this->EnvironmentPtr );
		if ( SELECT_CASE_var == GroundEnv ) {
			//Approximate conductance using ground conductivity, (h=k/L), where L is grid spacing
			// between pipe wall and next closest node.
			EnvHeatTransCoef = this->SoilConductivity / ( this->dSregular - ( this->PipeID / 2.0 ) );
		} else if ( SELECT_CASE_var == OutsideAirEnv ) {
			EnvHeatTransCoef = AirConvCoef;
		} else if ( SELECT_CASE_var == ZoneEnv ) {
			EnvHeatTransCoef = AirConvCoef;
		} else if ( SELECT_CASE_var == ScheduleEnv ) {
			EnvHeatTransCoef = AirConvCoef;
		} else if ( SELECT_CASE_var == None ) {
			EnvHeatTransCoef = 0.0;
		} else {
			EnvHeatTransCoef = 0.0;
		}}

		// work out the coefficients
		FluidNodeHeatCapacity = this->SectionArea * this->Length / this->NumSections * this->FluidSpecHeat * this->FluidDensity; // Mass of Node x Specific heat

		// coef of fluid heat balance
		A1 = FluidNodeHeatCapacity + MassFlowRate * this->FluidSpecHeat * DeltaTime + FluidConvCoef * this->InsideArea * DeltaTime;

		A2 = MassFlowRate * this->FluidSpecHeat * DeltaTime;

		A3 = FluidConvCoef * this->InsideArea * DeltaTime;

		A4 = FluidNodeHeatCapacity;

		// coef of pipe heat balance
		B1 = this->PipeHeatCapacity + FluidConvCoef * this->InsideArea * DeltaTime + EnvHeatTransCoef * this->OutsideArea * DeltaTime;

		B2 = A3;

		B3 = EnvHeatTransCoef * this->OutsideArea * DeltaTime;

		B4 = this->PipeHeatCapacity;

		this->TentativeFluidTemp( 0 ) = InletTemp;

		this->TentativePipeTemp( 0 ) = this->PipeTemp( 1 ); // for convenience

		if ( present( LengthIndex ) ) { //Just simulate the single section if being called from Pipe:Underground

			PipeDepth = this->PipeNodeDepth;
			PipeWidth = this->PipeNodeWidth;
			TempBelow = this->T( PipeWidth, PipeDepth + 1, LengthIndex, CurrentTimeIndex );
			TempBeside = this->T( PipeWidth - 1, PipeDepth, LengthIndex, CurrentTimeIndex );
			TempAbove = this->T( PipeWidth, PipeDepth - 1, LengthIndex, CurrentTimeIndex );
			EnvironmentTemp = ( TempBelow + TempBeside + TempAbove ) / 3.0;

			this->TentativeFluidTemp( LengthIndex ) = ( A2 * this->TentativeFluidTemp( LengthIndex - 1 ) + A3 / B1 * ( B3 * EnvironmentTemp + B4 * this->PreviousPipeTemp( LengthIndex ) ) + A4 * this->PreviousFluidTemp( LengthIndex ) ) / ( A1 - A3 * B2 / B1 );

			this->TentativePipeTemp( LengthIndex ) = ( B2 * this->TentativeFluidTemp( LengthIndex ) + B3 * EnvironmentTemp + B4 * this->PreviousPipeTemp( LengthIndex ) ) / B1;

			// Get exterior surface temperature from energy balance at the surface
			Numerator = EnvironmentTemp - this->TentativeFluidTemp( LengthIndex );
			Denominator = EnvHeatTransCoef * ( ( 1 / EnvHeatTransCoef ) + this->SumTK );
			SurfaceTemp = EnvironmentTemp - Numerator / Denominator;

			// keep track of environmental heat loss rate - not same as fluid loss at same time
			EnvHeatLossRate += EnvHeatTransCoef * this->OutsideArea * ( SurfaceTemp - EnvironmentTemp );

		} else { //Simulate all sections at once if not pipe:underground

			// start loop along pipe
			// b1 must not be zero but this should have been checked on input
			for ( curnode = 1; curnode <= this->NumSections; ++curnode ) {
				this->TentativeFluidTemp( curnode ) = ( A2 * this->TentativeFluidTemp( curnode - 1 ) + A3 / B1 * ( B3 * EnvironmentTemp + B4 * this->PreviousPipeTemp( curnode ) ) + A4 * this->PreviousFluidTemp( curnode ) ) / ( A1 - A3 * B2 / B1 );

				this->TentativePipeTemp( curnode ) = ( B2 * this->TentativeFluidTemp( curnode ) + B3 * EnvironmentTemp + B4 * this->PreviousPipeTemp( curnode ) ) / B1;

				// Get exterior surface temperature from energy balance at the surface
				Numerator = EnvironmentTemp - this->TentativeFluidTemp( curnode );
				Denominator = EnvHeatTransCoef * ( ( 1 / EnvHeatTransCoef ) + this->SumTK );
				SurfaceTemp = EnvironmentTemp - Numerator / Denominator;

				// Keep track of environmental heat loss
				EnvHeatLossRate += EnvHeatTransCoef * this->OutsideArea * ( SurfaceTemp - EnvironmentTemp );

			}

		}

		FluidHeatLossRate = MassFlowRate * this->FluidSpecHeat * ( this->TentativeFluidTemp( 0 ) - this->TentativeFluidTemp( this->NumSections ) );

		OutletTemp = this->TentativeFluidTemp( this->NumSections );

	}

	void
	PipeHTData::calcBuriedPipeSoil() {

		//       AUTHOR         Edwin Lee
		//       DATE WRITTEN   May 2008
		//       MODIFIED       na
		//       RE-ENGINEERED  na

		// PURPOSE OF THIS SUBROUTINE:
		// This subroutine does all of the stuff that is necessary to simulate
		// soil heat transfer with a Buried Pipe.

		// METHODOLOGY EMPLOYED:
		// An implicit pseudo 3D finite difference grid
		// is set up, which simulates transient behavior in the soil.
		// This then interfaces with the Hanby model for near-pipe region

		// REFERENCES: See Module Level Description

		// Using/Aliasing
		using DataLoopNode::Node;
		using DataEnvironment::OutDryBulbTemp;
		using DataEnvironment::SkyTemp;
		using DataEnvironment::WindSpeed;
		using DataEnvironment::BeamSolarRad;
		using DataEnvironment::DifSolarRad;
		using DataEnvironment::SOLCOS;
		using DataGlobals::Pi;
		using DataGlobals::TimeStep;
		using DataGlobals::HourOfDay;
		using DataGlobals::KelvinConv;
		using DataGlobals::rTinyValue;
		using ConvectionCoefficients::CalcASHRAESimpExtConvectCoeff;

		// SUBROUTINE ARGUMENT DEFINITIONS:

		// Locals
		// SUBROUTINE PARAMETER DEFINITIONS:
		int const NumSections( 20 );
		Real64 const ConvCrit( 0.05 );
		int const MaxIterations( 200 );
		Real64 const StefBoltzmann( 5.6697e-08 ); // Stefan-Boltzmann constant

		// SUBROUTINE LOCAL VARIABLE DECLARATIONS:
		static int IterationIndex( 0 ); // Index when stepping through equations
		static int LengthIndex( 0 ); // Index for nodes along length of pipe
		static int DepthIndex( 0 ); // Index for nodes in the depth direction
		static int WidthIndex( 0 ); // Index for nodes in the width direction
		static Real64 ConvCoef( 0.0 ); // Current convection coefficient = f(Wind Speed,Roughness)
		static Real64 RadCoef( 0.0 ); // Current radiation coefficient
		static Real64 QSolAbsorbed( 0.0 ); // Current total solar energy absorbed
		Array3D< Real64 > T_O( this->PipeNodeWidth, this->NumDepthNodes, NumSections );

		//Local variable placeholders for code readability
		static Real64 A1( 0.0 ); // Placeholder for CoefA1
		static Real64 A2( 0.0 ); // Placeholder for CoefA2
		static Real64 NodeBelow( 0.0 ); // Placeholder for Node temp below current node
		static Real64 NodeAbove( 0.0 ); // Placeholder for Node temp above current node
		static Real64 NodeRight( 0.0 ); // Placeholder for Node temp to the right of current node
		static Real64 NodeLeft( 0.0 ); // Placeholder for Node temp to the left of current node
		static Real64 NodePast( 0.0 ); // Placeholder for Node temp at current node but previous time step
		static Real64 PastNodeTempAbs( 0.0 ); // Placeholder for absolute temperature (K) version of NodePast
		static Real64 Ttemp( 0.0 ); // Placeholder for a current temperature node in convergence check
		static Real64 SkyTempAbs( 0.0 ); // Placeholder for current sky temperature in Kelvin
		static int TopRoughness( 0 ); // Placeholder for soil surface roughness
		static Real64 TopThermAbs( 0.0 ); // Placeholder for soil thermal radiation absorptivity
		static Real64 TopSolarAbs( 0.0 ); // Placeholder for soil solar radiation absorptivity
		static Real64 kSoil( 0.0 ); // Placeholder for soil conductivity
		static Real64 dS( 0.0 ); // Placeholder for soil grid spacing
		static Real64 rho( 0.0 ); // Placeholder for soil density
		static Real64 Cp( 0.0 ); // Placeholder for soil specific heat

		// There are a number of coefficients which change through the simulation, and they are updated here
		this->FourierDS = this->SoilDiffusivity * DeltaTime / pow_2( this->dSregular ); //Eq. D4
		this->CoefA1 = this->FourierDS / ( 1 + 4 * this->FourierDS ); //Eq. D2
		this->CoefA2 = 1 / ( 1 + 4 * this->FourierDS ); //Eq. D3

		for ( IterationIndex = 1; IterationIndex <= MaxIterations; ++IterationIndex ) {
			if ( IterationIndex == MaxIterations ) {
				ShowWarningError( "BuriedPipeHeatTransfer: Large number of iterations detected in object: " + this->Name );
			}

			//Store computed values in T_O array
			for ( LengthIndex = 2; LengthIndex <= this->NumSections; ++LengthIndex ) {
				for ( DepthIndex = 1; DepthIndex <= this->NumDepthNodes - 1; ++DepthIndex ) {
					for ( WidthIndex = 2; WidthIndex <= this->PipeNodeWidth; ++WidthIndex ) {
						T_O( WidthIndex, DepthIndex, LengthIndex ) = this->T( WidthIndex, DepthIndex, LengthIndex, TentativeTimeIndex );
					}
				}
			}

			//Loop along entire length of pipe, analyzing cross sects
			for ( LengthIndex = 1; LengthIndex <= this->NumSections; ++LengthIndex ) {
				for ( DepthIndex = 1; DepthIndex <= this->NumDepthNodes - 1; ++DepthIndex ) {
					for ( WidthIndex = 2; WidthIndex <= this->PipeNodeWidth; ++WidthIndex ) {

						if ( DepthIndex == 1 ) { //Soil Surface Boundary

							//If on soil boundary, load up local variables and perform calculations
							NodePast = this->T( WidthIndex, DepthIndex, LengthIndex, PreviousTimeIndex );
							PastNodeTempAbs = NodePast + KelvinConv;
							SkyTempAbs = SkyTemp + KelvinConv;
							TopRoughness = this->SoilRoughness;
							TopThermAbs = this->SoilThermAbs;
							TopSolarAbs = this->SoilSolarAbs;
							kSoil = this->SoilConductivity;
							dS = this->dSregular;
							rho = this->SoilDensity;
							Cp = this->SoilCp;

							// ASHRAE simple convection coefficient model for external surfaces.
							this->OutdoorConvCoef = CalcASHRAESimpExtConvectCoeff( TopRoughness, WindSpeed );
							ConvCoef = this->OutdoorConvCoef;

							// thermal radiation coefficient using surf temp from past time step
							if ( std::abs( PastNodeTempAbs - SkyTempAbs ) > rTinyValue ) {
								RadCoef = StefBoltzmann * TopThermAbs * ( pow_4( PastNodeTempAbs ) - pow_4( SkyTempAbs ) ) / ( PastNodeTempAbs - SkyTempAbs );
							} else {
								RadCoef = 0.0;
							}

							// total absorbed solar - no ground solar
							QSolAbsorbed = TopSolarAbs * ( max( SOLCOS( 3 ), 0.0 ) * BeamSolarRad + DifSolarRad );

							// If sun is not exposed, then turn off both solar and thermal radiation
							if ( ! this->SolarExposed ) {
								RadCoef = 0.0;
								QSolAbsorbed = 0.0;
							}

							if ( WidthIndex == this->PipeNodeWidth ) { //Symmetric centerline boundary

								//-Coefficients and Temperatures
								NodeBelow = this->T( WidthIndex, DepthIndex + 1, LengthIndex, CurrentTimeIndex );
								NodeLeft = this->T( WidthIndex - 1, DepthIndex, LengthIndex, CurrentTimeIndex );

								//-Update Equation, basically a detailed energy balance at the surface
								this->T( WidthIndex, DepthIndex, LengthIndex, TentativeTimeIndex ) = ( QSolAbsorbed + RadCoef * SkyTemp + ConvCoef * OutDryBulbTemp + ( kSoil / dS ) * ( NodeBelow + 2 * NodeLeft ) + ( rho * Cp / DeltaTime ) * NodePast ) / ( RadCoef + ConvCoef + 3 * ( kSoil / dS ) + ( rho * Cp / DeltaTime ) );

							} else { //Soil surface, but not on centerline

								//-Coefficients and Temperatures
								NodeBelow = this->T( WidthIndex, DepthIndex + 1, LengthIndex, CurrentTimeIndex );
								NodeLeft = this->T( WidthIndex - 1, DepthIndex, LengthIndex, CurrentTimeIndex );
								NodeRight = this->T( WidthIndex + 1, DepthIndex, LengthIndex, CurrentTimeIndex );

								//-Update Equation
								this->T( WidthIndex, DepthIndex, LengthIndex, TentativeTimeIndex ) = ( QSolAbsorbed + RadCoef * SkyTemp + ConvCoef * OutDryBulbTemp + ( kSoil / dS ) * ( NodeBelow + NodeLeft + NodeRight ) + ( rho * Cp / DeltaTime ) * NodePast ) / ( RadCoef + ConvCoef + 3 * ( kSoil / dS ) + ( rho * Cp / DeltaTime ) );

							} //Soil-to-air surface node structure

						} else if ( WidthIndex == this->PipeNodeWidth ) { //On Symmetric centerline boundary

							if ( DepthIndex == this->PipeNodeDepth ) { //On the node containing the pipe

								//-Call to simulate a single pipe segment (by passing OPTIONAL LengthIndex argument)
								CalcPipesHeatTransfer( PipeHTNum, LengthIndex );

								//-Update node for cartesian system
								this->T( WidthIndex, DepthIndex, LengthIndex, TentativeTimeIndex ) = this->PipeTemp( LengthIndex );

							} else if ( DepthIndex != 1 ) { //Not surface node

								//-Coefficients and Temperatures
								NodeLeft = this->T( WidthIndex - 1, DepthIndex, LengthIndex, CurrentTimeIndex );
								NodeAbove = this->T( WidthIndex, DepthIndex - 1, LengthIndex, CurrentTimeIndex );
								NodeBelow = this->T( WidthIndex, DepthIndex + 1, LengthIndex, CurrentTimeIndex );
								NodePast = this->T( WidthIndex, DepthIndex, LengthIndex, CurrentTimeIndex - 1 );
								A1 = this->CoefA1;
								A2 = this->CoefA2;

								//-Update Equation
								this->T( WidthIndex, DepthIndex, LengthIndex, TentativeTimeIndex ) = A1 * ( NodeBelow + NodeAbove + 2 * NodeLeft ) + A2 * NodePast;

							} //Symmetric centerline node structure

						} else { //All Normal Interior Nodes

							//-Coefficients and Temperatures
							A1 = this->CoefA1;
							A2 = this->CoefA2;
							NodeBelow = this->T( WidthIndex, DepthIndex + 1, LengthIndex, CurrentTimeIndex );
							NodeAbove = this->T( WidthIndex, DepthIndex - 1, LengthIndex, CurrentTimeIndex );
							NodeRight = this->T( WidthIndex + 1, DepthIndex, LengthIndex, CurrentTimeIndex );
							NodeLeft = this->T( WidthIndex - 1, DepthIndex, LengthIndex, CurrentTimeIndex );
							NodePast = this->T( WidthIndex, DepthIndex, LengthIndex, CurrentTimeIndex - 1 );

							//-Update Equation
							this->T( WidthIndex, DepthIndex, LengthIndex, TentativeTimeIndex ) = A1 * ( NodeBelow + NodeAbove + NodeRight + NodeLeft ) + A2 * NodePast; //Eq. D1

						}
					}
				}
			}

			//Check for convergence
			for ( LengthIndex = 2; LengthIndex <= this->NumSections; ++LengthIndex ) {
				for ( DepthIndex = 1; DepthIndex <= this->NumDepthNodes - 1; ++DepthIndex ) {
					for ( WidthIndex = 2; WidthIndex <= this->PipeNodeWidth; ++WidthIndex ) {
						Ttemp = this->T( WidthIndex, DepthIndex, LengthIndex, TentativeTimeIndex );
						if ( std::abs( T_O( WidthIndex, DepthIndex, LengthIndex ) - Ttemp ) > ConvCrit ) goto IterationLoop_loop;
					}
				}
			}

			//If we didn't cycle back, then the system is converged
			goto IterationLoop_exit;

			IterationLoop_loop: ;
		}
		IterationLoop_exit: ;

	}

	void
	PipeHTData::updatePipesHeatTransfer() {
		
		int const OutletNodeNum = this->OutletNodeNum
		int const InletNodeNum = this->InletNodeNum
		
		Node( OutletNodeNum ).Temp = this->OutletTemp;

		// pass everything else through
		Node( OutletNodeNum ).TempMin = Node( InletNodeNum ).TempMin;
		Node( OutletNodeNum ).TempMax = Node( InletNodeNum ).TempMax;
		Node( OutletNodeNum ).MassFlowRate = Node( InletNodeNum ).MassFlowRate;
		Node( OutletNodeNum ).MassFlowRateMin = Node( InletNodeNum ).MassFlowRateMin;
		Node( OutletNodeNum ).MassFlowRateMax = Node( InletNodeNum ).MassFlowRateMax;
		Node( OutletNodeNum ).MassFlowRateMinAvail = Node( InletNodeNum ).MassFlowRateMinAvail;
		Node( OutletNodeNum ).MassFlowRateMaxAvail = Node( InletNodeNum ).MassFlowRateMaxAvail;
		Node( OutletNodeNum ).Quality = Node( InletNodeNum ).Quality;
		//Only pass pressure if we aren't doing a pressure simulation
		if ( PlantLoop( this->LoopNum ).PressureSimType > 1 ) {
			//Don't do anything
		} else {
			Node( OutletNodeNum ).Press = Node( InletNodeNum ).Press;
		}
		Node( OutletNodeNum ).Enthalpy = Node( InletNodeNum ).Enthalpy;
		Node( OutletNodeNum ).HumRat = Node( InletNodeNum ).HumRat;
	}

	void
	PipeHTData::ReportPipesHeatTransfer() {
		// update flows and temps from module variables
		this->FluidInletTemp = this->InletTemp;
		this->FluidOutletTemp = this->OutletTemp;
		this->MassFlowRate = this->MassFlowRate;
		this->VolumeFlowRate = this->VolumeFlowRate;

		// update other variables from module variables
		this->FluidHeatLossRate = this->FluidHeatLossRate;
		this->FluidHeatLossEnergy = this->FluidHeatLossRate * this->DeltaTime; // DeltaTime is in seconds
		this->PipeInletTemp = this->PipeTemp( 1 );
		this->PipeOutletTemp = this->PipeTemp( this->NumSections );

		// need to average the heat rate because it is now summing over multiple inner time steps
		this->EnvironmentHeatLossRate = this->EnvHeatLossRate / this->NumInnerTimeSteps;
		this->EnvHeatLossEnergy = this->EnvironmentHeatLossRate * this->DeltaTime;

		// for zone heat gains, we assign the averaged heat rate over all inner time steps
		if ( this->EnvironmentPtr == ZoneEnv ) {
			this->ZoneHeatGainRate = this->EnvironmentHeatLossRate;
		}
	}

	Real64
	PipeHTData::calcPipeHeatTransCoef(
		Real64 const Temperature, // Temperature of water entering the surface, in C
		Real64 const MassFlowRate, // Mass flow rate, in kg/s
		Real64 const Diameter // Pipe diameter, m
	) {
		
		// FUNCTION INFORMATION:
		//       AUTHOR         Simon Rees
		//       DATE WRITTEN   July 2007
		//       MODIFIED       na
		//       RE-ENGINEERED  na

		// PURPOSE OF THIS SUBROUTINE:
		// This subroutine calculates pipe/fluid heat transfer coefficients.
		// This routine is adapted from that in the low temp radiant surface model.

		// METHODOLOGY EMPLOYED:
		// Currently assumes water data when calculating Pr and Re

		// REFERENCES:
		// See RadiantSystemLowTemp module.
		// Property data for water shown below as parameters taken from
		// Incropera and DeWitt, Introduction to Heat Transfer, Table A.6.
		// Heat exchanger information also from Incropera and DeWitt.
		// Code based loosely on code from IBLAST program (research version)

		// Using/Aliasing
		using DataGlobals::Pi;
		using DataPlant::PlantLoop;
		using FluidProperties::GetConductivityGlycol;
		using FluidProperties::GetViscosityGlycol;

		// Return value
		Real64 CalcPipeHeatTransCoef;

		// Locals
		// SUBROUTINE ARGUMENT DEFINITIONS:

		// SUBROUTINE PARAMETER DEFINITIONS:
		static std::string const RoutineName( "PipeHeatTransfer::CalcPipeHeatTransCoef: " );
		Real64 const MaxLaminarRe( 2300.0 ); // Maximum Reynolds number for laminar flow
		int const NumOfPropDivisions( 13 ); // intervals in property correlation
		static Array1D< Real64 > const Temps( NumOfPropDivisions, { 1.85, 6.85, 11.85, 16.85, 21.85, 26.85, 31.85, 36.85, 41.85, 46.85, 51.85, 56.85, 61.85 } ); // Temperature, in C
		static Array1D< Real64 > const Mu( NumOfPropDivisions, { 0.001652, 0.001422, 0.001225, 0.00108, 0.000959, 0.000855, 0.000769, 0.000695, 0.000631, 0.000577, 0.000528, 0.000489, 0.000453 } ); // Viscosity, in Ns/m2
		static Array1D< Real64 > const Conductivity( NumOfPropDivisions, { 0.574, 0.582, 0.590, 0.598, 0.606, 0.613, 0.620, 0.628, 0.634, 0.640, 0.645, 0.650, 0.656 } ); // Conductivity, in W/mK
		static Array1D< Real64 > const Pr( NumOfPropDivisions, { 12.22, 10.26, 8.81, 7.56, 6.62, 5.83, 5.20, 4.62, 4.16, 3.77, 3.42, 3.15, 2.88 } ); // Prandtl number (dimensionless)

		// INTERFACE BLOCK SPECIFICATIONS
		// na

		// DERIVED TYPE DEFINITIONS
		// na

		// SUBROUTINE LOCAL VARIABLE DECLARATIONS:
		int idx;
		Real64 InterpFrac;
		Real64 NuD;
		Real64 ReD;
		Real64 Kactual;
		Real64 MUactual;
		Real64 PRactual;
		int LoopNum;

		//retrieve loop index for this component so we can look up fluid properties
		LoopNum = this->LoopNum;

		//since the fluid properties routine doesn't have Prandtl, we'll just use water values
		idx = 1;
		while ( idx <= NumOfPropDivisions ) {
			if ( Temperature < Temps( idx ) ) {
				if ( idx == 1 ) {
					PRactual = Pr( idx );
				} else if ( idx > NumOfPropDivisions ) {
					PRactual = Pr( NumOfPropDivisions ); //CR 8566
				} else {
					InterpFrac = ( Temperature - Temps( idx - 1 ) ) / ( Temps( idx ) - Temps( idx - 1 ) );
					PRactual = Pr( idx - 1 ) + InterpFrac * ( Pr( idx ) - Pr( idx - 1 ) );
				}
				break; // DO loop
			} else { //CR 8566
				PRactual = Pr( NumOfPropDivisions );
			}
			++idx;
		}

		//look up conductivity and viscosity
		Kactual = GetConductivityGlycol( PlantLoop( LoopNum ).FluidName, this->FluidTemp( 0 ), PlantLoop( LoopNum ).FluidIndex, RoutineName ); //W/m-K
		MUactual = GetViscosityGlycol( PlantLoop( LoopNum ).FluidName, this->FluidTemp( 0 ), PlantLoop( LoopNum ).FluidIndex, RoutineName ) / 1000.0; //Note fluid properties routine returns mPa-s, we need Pa-s

		// Calculate the Reynold's number from RE=(4*Mdot)/(Pi*Mu*Diameter) - as RadiantSysLowTemp
		ReD = 4.0 * MassFlowRate / ( Pi * MUactual * Diameter );

		if ( ReD == 0.0 ) { // No flow

			//For now just leave it how it was doing it before
			NuD = 3.66;
			//Although later it would be nice to have a natural convection correlation

		} else { // Calculate the Nusselt number based on what flow regime one is in

			if ( ReD >= MaxLaminarRe ) { // Turbulent flow --> use Colburn equation
				NuD = 0.023 * std::pow( ReD, 0.8 ) * std::pow( PRactual, 1.0 / 3.0 );
			} else { // Laminar flow --> use constant surface temperature relation
				NuD = 3.66;
			}

		}

		CalcPipeHeatTransCoef = Kactual * NuD / Diameter;

		return CalcPipeHeatTransCoef;

	}

	Real64
	PipeHTData::outsidePipeHeatTransCoef() {

		// FUNCTION INFORMATION:
		//       AUTHOR         Dan Fisher
		//       DATE WRITTEN   July 2007
		//       MODIFIED       na
		//       RE-ENGINEERED  na

		// PURPOSE OF THIS SUBROUTINE:
		// This subroutine calculates the convection heat transfer
		// coefficient for a cylinder in cross flow.

		// REFERENCES:
		// Fundamentals of Heat and Mass Transfer: Incropera and DeWitt, 4th ed.
		// p. 369-370 (Eq. 7:55b)

		// Using/Aliasing
		using DataHeatBalFanSys::MAT; // average (mean) zone air temperature [C]
		using DataLoopNode::Node;
		using ScheduleManager::GetCurrentScheduleValue;
		using DataEnvironment::WindSpeed;

		// Return value
		Real64 OutsidePipeHeatTransCoef;

		// Locals
		// SUBROUTINE ARGUMENT DEFINITIONS:

		// SUBROUTINE PARAMETER DEFINITIONS:
		Real64 const Pr( 0.7 ); // Prandl number for air (assume constant)
		Real64 const CondAir( 0.025 ); // thermal conductivity of air (assume constant) [W/m.K]
		Real64 const RoomAirVel( 0.381 ); // room air velocity of 75 ft./min [m/s]
		Real64 const NaturalConvNusselt( 0.36 );
		//Nusselt for natural convection for horizontal cylinder
		//from: Correlations for Convective Heat Transfer
		//      Dr. Bernhard Spang
		//      Chemical Engineers’ Resource Page: http://www.cheresources.com/convection.pdf
		int const NumOfParamDivisions( 5 ); // intervals in property correlation
		int const NumOfPropDivisions( 12 ); // intervals in property correlation

		static Array1D< Real64 > const CCoef( NumOfParamDivisions, { 0.989, 0.911, 0.683, 0.193, 0.027 } ); // correlation coefficient
		static Array1D< Real64 > const mExp( NumOfParamDivisions, { 0.33, 0.385, 0.466, 0.618, 0.805 } ); // exponent
		static Array1D< Real64 > const LowerBound( NumOfParamDivisions, { 0.4, 4.0, 40.0, 4000.0, 40000.0 } ); // upper bound of correlation range
		static Array1D< Real64 > const UpperBound( NumOfParamDivisions, { 4.0, 40.0, 4000.0, 40000.0, 400000.0 } ); // lower bound of correlation range

		static Array1D< Real64 > const Temperature( NumOfPropDivisions, { -73.0, -23.0, -10.0, 0.0, 10.0, 20.0, 27.0, 30.0, 40.0, 50.0, 76.85, 126.85 } ); // temperature [C]
		static Array1D< Real64 > const DynVisc( NumOfPropDivisions, { 75.52e-7, 11.37e-6, 12.44e-6, 13.3e-6, 14.18e-6, 15.08e-6, 15.75e-6, 16e-6, 16.95e-6, 17.91e-6, 20.92e-6, 26.41e-6 } ); // dynamic viscosity [m^2/s]

		// INTERFACE BLOCK SPECIFICATIONS
		// na

		// DERIVED TYPE DEFINITIONS
		// na

		// SUBROUTINE LOCAL VARIABLE DECLARATIONS:
		int idx;
		Real64 NuD;
		Real64 ReD;
		Real64 Coef;
		Real64 rExp;
		Real64 AirVisc;
		Real64 AirVel;
		Real64 AirTemp;
		Real64 PipeOD;
		bool ViscositySet;
		bool CoefSet;

		//Set environmental variables
		{ auto const SELECT_CASE_var( this->TypeOf );

		if ( SELECT_CASE_var == TypeOf_PipeInterior ) {

			{ auto const SELECT_CASE_var1( this->EnvironmentPtr );
			if ( SELECT_CASE_var1 == ScheduleEnv ) {
				AirTemp = GetCurrentScheduleValue( this->EnvrSchedPtr );
				AirVel = GetCurrentScheduleValue( this->EnvrVelSchedPtr );

			} else if ( SELECT_CASE_var1 == ZoneEnv ) {
				AirTemp = MAT( this->EnvrZonePtr );
				AirVel = RoomAirVel;
			}}

		} else if ( SELECT_CASE_var == TypeOf_PipeExterior ) {

			{ auto const SELECT_CASE_var1( this->EnvironmentPtr );
			if ( SELECT_CASE_var1 == OutsideAirEnv ) {
				AirTemp = Node( this->EnvrAirNodeNum ).Temp;
				AirVel = WindSpeed;
			}}

		}}

		PipeOD = this->InsulationOD;

		ViscositySet = false;
		for ( idx = 1; idx <= NumOfPropDivisions; ++idx ) {
			if ( AirTemp <= Temperature( idx ) ) {
				AirVisc = DynVisc( idx );
				ViscositySet = true;
				break;
			}
		}

		if ( ! ViscositySet ) {
			AirVisc = DynVisc( NumOfPropDivisions );
			if ( AirTemp > Temperature( NumOfPropDivisions ) ) {
				ShowWarningError( "Heat Transfer Pipe = " + this->Name + "Viscosity out of range, air temperature too high, setting to upper limit." );
			}
		}

		// Calculate the Reynold's number
		CoefSet = false;
		if ( AirVisc > 0.0 ) {
			ReD = AirVel * PipeOD / ( AirVisc );
		}

		for ( idx = 1; idx <= NumOfParamDivisions; ++idx ) {
			if ( ReD <= UpperBound( idx ) ) {
				Coef = CCoef( idx );
				rExp = mExp( idx );
				CoefSet = true;
				break;
			}
		}

		if ( ! CoefSet ) {
			Coef = CCoef( NumOfParamDivisions );
			rExp = mExp( NumOfParamDivisions );
			if ( ReD > UpperBound( NumOfParamDivisions ) ) {
				ShowWarningError( "Heat Transfer Pipe = " + this->Name + "Reynolds Number out of range, setting coefficients to upper limit." );
			}
		}

		// Calculate the Nusselt number
		NuD = Coef * std::pow( ReD, rExp ) * std::pow( Pr, 1.0 / 3.0 );

		// If the wind speed is too small, we need to use natural convection behavior:
		NuD = max( NuD, NaturalConvNusselt );

		// h = (k)(Nu)/D
		OutsidePipeHeatTransCoef = CondAir * NuD / PipeOD;

		return OutsidePipeHeatTransCoef;

	}

	Real64
	PipeHTData::TBND(
		Real64 const z, // Current Depth
		Real64 const DayOfSim, // Current Simulation Day
	) {
		using DataGlobals::Pi;

		// Return value
		Real64 TBND;

		//Kusuda and Achenbach
		TBND = this->AvgGroundTemp - this->AvgGndTempAmp * std::exp( -z * std::sqrt( Pi / ( 365.0 * this->SoilDiffusivityPerDay ) ) ) * std::cos( ( 2.0 * Pi / 365.0 ) * ( DayOfSim - this->PhaseShiftDays - ( z / 2.0 ) * std::sqrt( 365.0 / ( Pi * this->SoilDiffusivityPerDay ) ) ) );

		return TBND;

	}

	//     NOTICE

	//     Copyright © 1996-2014 The Board of Trustees of the University of Illinois
	//     and The Regents of the University of California through Ernest Orlando Lawrence
	//     Berkeley National Laboratory.  All rights reserved.

	//     Portions of the EnergyPlus software package have been developed and copyrighted
	//     by other individuals, companies and institutions.  These portions have been
	//     incorporated into the EnergyPlus software package under license.   For a complete
	//     list of contributors, see "Notice" located in main.cc.

	//     NOTICE: The U.S. Government is granted for itself and others acting on its
	//     behalf a paid-up, nonexclusive, irrevocable, worldwide license in this data to
	//     reproduce, prepare derivative works, and perform publicly and display publicly.
	//     Beginning five (5) years after permission to assert copyright is granted,
	//     subject to two possible five year renewals, the U.S. Government is granted for
	//     itself and others acting on its behalf a paid-up, non-exclusive, irrevocable
	//     worldwide license in this data to reproduce, prepare derivative works,
	//     distribute copies to the public, perform publicly and display publicly, and to
	//     permit others to do so.

	//     TRADEMARKS: EnergyPlus is a trademark of the US Department of Energy.

} // PipeHeatTransfer

} // EnergyPlus
