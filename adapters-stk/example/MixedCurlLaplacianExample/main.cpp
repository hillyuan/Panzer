// @HEADER
// ***********************************************************************
//
//           TianXin: A partial differential equation assembly
//       engine for strongly coupled complex multiphysics systems
//                 Copyright (2022) Xi Yuan
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// ***********************************************************************
// @HEADER

#include <Teuchos_ConfigDefs.hpp>
#include <Teuchos_UnitTestHarness.hpp>
#include <Teuchos_RCP.hpp>
#include <Teuchos_TimeMonitor.hpp>
#include <Teuchos_StackedTimer.hpp>
#include <Teuchos_FancyOStream.hpp>

#include "Teuchos_DefaultComm.hpp"
#include "Teuchos_GlobalMPISession.hpp"
#include "Teuchos_CommandLineProcessor.hpp"

#include "PanzerAdaptersSTK_config.hpp"
#include "Panzer_GlobalData.hpp"
#include "Panzer_Workset_Builder.hpp"
#include "Panzer_WorksetContainer.hpp"
#include "Panzer_AssemblyEngine.hpp"
#include "Panzer_AssemblyEngine_TemplateManager.hpp"
#include "Panzer_AssemblyEngine_TemplateBuilder.hpp"
#include "Panzer_LinearObjFactory.hpp"
#include "Panzer_TpetraLinearObjFactory.hpp"
#include "Panzer_DOFManagerFactory.hpp"
#include "Panzer_FieldManagerBuilder.hpp"
#include "Panzer_PureBasis.hpp"
#include "Panzer_GlobalData.hpp"
#include "Panzer_ResponseLibrary.hpp"
#include "Panzer_ResponseEvaluatorFactory_Functional.hpp"
#include "Panzer_Response_Functional.hpp"
#include "Panzer_CheckBCConsistency.hpp"

#include "PanzerAdaptersSTK_config.hpp"
#include "Panzer_STK_WorksetFactory.hpp"
#include "Panzer_STKConnManager.hpp"
#include "Panzer_STK_Interface.hpp"
#include "Panzer_STK_SquareQuadMeshFactory.hpp"
#include "Panzer_STK_SquareTriMeshFactory.hpp"
#include "Panzer_STK_CubeHexMeshFactory.hpp"
#include "Panzer_STK_SetupUtilities.hpp"

#include "BelosPseudoBlockGmresSolMgr.hpp"
#include "BelosTpetraAdapter.hpp"

#include "Example_ClosureModel_Factory_TemplateBuilder.hpp"
#include "Example_EquationSetFactory.hpp"

#include <sstream>

using Teuchos::RCP;
using Teuchos::rcp;


//
// This is the Mathematica code used to generate this example.
// It also generates a plot of the vector field so it is clear
// what the solution is doing.
//
//      Needs["VectorAnalysis`"]
//
//      phi0[x_,y_]=(1-x)*(1-y)
//      phi1[x_,y_]=x*(1-y)
//      phi2[x_,y_]=x*y
//      phi3[x_,y_]=y*(1-x)
//
//      psi0[x_,y_]={1-y,0,0}
//      psi1[x_,y_]={0,x,0}
//      psi2[x_,y_]={y,0,0}
//      psi3[x_,y_]={0,1-x,0}
//
//      u[x_,y_]=phi2[x,y]*psi0[x,y]+phi3[x,y]*psi1[x,y]+phi0[x,y]*psi2[x,y]+phi1[x,y]*psi3[x,y]
//      f[x_,y_]=u[x,y]+Curl[Curl[u[x,y],Cartesian[x,y,z]],Cartesian[x,y,z]]
//
//      TwoDVec[g_]={g[[1]],g[[2]]}
//
//      DotProduct[u[0.5,0],{1,0,0}]
//      DotProduct[u[1,0.5],{0,1,0}]
//      DotProduct[u[0.5,1],{1,0,0}]
//      DotProduct[u[0,0.5],{0,1,0}]
//
//      Out[118]= 0.
//      Out[119]= 0.
//      Out[120]= 0.
//      Out[121]= 0.
//
//      VectorPlot[TwoDVec[u[x,y]],{x,0,1},{y,0,1}]
//      Simplify[u[x,y]]
//      Simplify[f[x,y]]
//
//      Out[144]= {-(-1+y) y,-(-1+x) x,0}
//      Out[145]= {2+y-y^2,2+x-x^2,0}
//

void solveTpetraSystem(panzer::LinearObjContainer & container);

int main(int argc,char * argv[])
{
   using Teuchos::RCP;
   using panzer::StrPureBasisPair;
   using panzer::StrPureBasisComp;

   Teuchos::GlobalMPISession mpiSession(&argc,&argv);
   Kokkos::initialize(argc,argv);
   Teuchos::RCP<const Teuchos::MpiComm<int> > comm = Teuchos::rcp(new Teuchos::MpiComm<int>(MPI_COMM_WORLD));
   Teuchos::FancyOStream out(Teuchos::rcpFromRef(std::cout));
   out.setOutputToRootOnly(0);
   out.setShowProcRank(true);

   const auto stackedTimer = Teuchos::rcp(new Teuchos::StackedTimer("Panzer MixedPoisson Test"));
   Teuchos::TimeMonitor::setStackedTimer(stackedTimer);
   stackedTimer->start("Curl Laplacian");

   // Build command line processor
   ////////////////////////////////////////////////////

   bool threeD = false;
   int x_blocks = 1;
   int x_elements=20,y_elements=20,z_elements=20;
   std::string celltype = "Quad"; // or "Tri" (2d), Hex or Tet (3d)
   double x_size=1.,y_size=1.,z_size=1.;
   int basis_order = 1;
   std::string output_filename="output_";

   Teuchos::CommandLineProcessor clp;
   clp.throwExceptions(false);
   clp.setDocString("This example solves curl laplacian problem with Hex and Tet inline mesh with high order.\n");

   clp.setOption("cell",&celltype);
   clp.setOption("use-threed","use-twod",&threeD);
   clp.setOption("x-blocks",&x_blocks);
   clp.setOption("x-elements",&x_elements);
   clp.setOption("y-elements",&y_elements);
   clp.setOption("z-elements",&z_elements);
   clp.setOption("x-size",&x_size);
   clp.setOption("y-size",&y_size);
   clp.setOption("z-size",&z_size);
   clp.setOption("basis-order",&basis_order);
   clp.setOption("output-filename",&output_filename);

   // parse commandline argument
   Teuchos::CommandLineProcessor::EParseCommandLineReturn r_parse= clp.parse( argc, argv );
   if (r_parse == Teuchos::CommandLineProcessor::PARSE_HELP_PRINTED) return  0;
   if (r_parse != Teuchos::CommandLineProcessor::PARSE_SUCCESSFUL  ) return -1;

   // variable declarations
   ////////////////////////////////////////////////////

   // factory definitions
   Teuchos::RCP<Example::EquationSetFactory> eqset_factory =
     Teuchos::rcp(new Example::EquationSetFactory); // where poison equation is defined

   // construction of uncommitted (no elements) mesh
   ////////////////////////////////////////////////////////

   RCP<panzer_stk::STK_MeshFactory> mesh_factory;
   if(threeD) {
     mesh_factory = rcp(new panzer_stk::CubeHexMeshFactory);

     // set mesh factory parameters
     RCP<Teuchos::ParameterList> pl = rcp(new Teuchos::ParameterList);
     pl->set("X Blocks",x_blocks);
     pl->set("Y Blocks",1);
     pl->set("Z Blocks",1);
     pl->set("X Elements",x_elements/x_blocks);
     pl->set("Y Elements",y_elements);
     pl->set("Z Elements",z_elements);
     pl->set("Xf",x_size);
     pl->set("Yf",y_size);
     pl->set("Zf",z_size);
     mesh_factory->setParameterList(pl);
   }
   else {
     if      (celltype == "Quad") mesh_factory = Teuchos::rcp(new panzer_stk::SquareQuadMeshFactory);
     else if (celltype == "Tri")  mesh_factory = Teuchos::rcp(new panzer_stk::SquareTriMeshFactory);
     else
       throw std::runtime_error("not supported celltype argument: try Quad or Tri");

     // set mesh factory parameters
     RCP<Teuchos::ParameterList> pl = rcp(new Teuchos::ParameterList);
     pl->set("X Blocks",x_blocks);
     pl->set("Y Blocks",1);
     pl->set("X Elements",x_elements/x_blocks);
     pl->set("Y Elements",y_elements);
     pl->set("Xf",x_size);
     pl->set("Yf",y_size);
     mesh_factory->setParameterList(pl);
   }

   RCP<panzer_stk::STK_Interface> mesh = mesh_factory->buildUncommitedMesh(MPI_COMM_WORLD);

   // other declarations
   const std::size_t workset_size = 8;

   // construct input physics and physics block
   ////////////////////////////////////////////////////////

   Teuchos::RCP<Teuchos::ParameterList> ipb = Teuchos::parameterList("Physics Blocks");
   std::vector<panzer::BC> bcs;
   std::vector<RCP<panzer::PhysicsBlock> > physicsBlocks;
   {
      bool build_transient_support = false;

      std::vector<std::string> eBlockNames;
      mesh->getElementBlockNames(eBlockNames);
	  
      {
		std::string b_basis_type = threeD ? "HDiv" : "HVol";
		int b_basis_order = threeD ? basis_order : basis_order-1; 

		Teuchos::ParameterList& p = ipb->sublist("CurlLapacian Physics");
		p.set("Type","CurlLaplacian");
		p.set("Model ID","solid");
		p.set("EField Basis Type","HCurl");
		p.set("BField Basis Type",b_basis_type);
		p.set("EField Basis Order",basis_order);
		p.set("BField Basis Order",b_basis_order);
		p.set("Integration Order",10);
	  }

      const panzer::CellData volume_cell_data(workset_size, mesh->getCellTopology(eBlockNames[0]));

      // GobalData sets ostream and parameter interface to physics
      Teuchos::RCP<panzer::GlobalData> gd = panzer::createGlobalData();

      // Can be overridden by the equation set
      int default_integration_order = 4;

      // the physics block knows how to build and register evaluator with the field manager
      for (const auto& block : eBlockNames) {
        RCP<panzer::PhysicsBlock> pb
          = rcp(new panzer::PhysicsBlock(ipb, block,
                                         default_integration_order,
                                         volume_cell_data,
                                         eqset_factory,
                                         gd,
                                         build_transient_support));

        // we can have more than one physics block, one per element block
        physicsBlocks.push_back(pb);
      }
   }

   // finish building mesh, set required field variables and mesh bulk data
   ////////////////////////////////////////////////////////////////////////

   for (const auto& pb : physicsBlocks) {
      const std::vector<StrPureBasisPair> & blockFields = pb->getProvidedDOFs();

      // insert all fields into a set
      std::set<StrPureBasisPair,StrPureBasisComp> fieldNames;
      fieldNames.insert(blockFields.begin(),blockFields.end());

      // build string for modifiying vectors
      std::vector<std::string> dimenStr(3);
      dimenStr[0] = "X"; dimenStr[1] = "Y"; dimenStr[2] = "Z";

      // add basis to DOF manager: block specific
      std::set<StrPureBasisPair,StrPureBasisComp>::const_iterator fieldItr;
      for (fieldItr=fieldNames.begin();fieldItr!=fieldNames.end();++fieldItr) {
         Teuchos::RCP<const panzer::PureBasis> basis = fieldItr->second;
         if(basis->getElementSpace()==panzer::PureBasis::HGRAD)
            mesh->addSolutionField(fieldItr->first,pb->elementBlockID());
         else if(basis->getElementSpace()==panzer::PureBasis::HCURL) {
            for(int i=0;i<basis->dimension();i++)
               mesh->addCellField(fieldItr->first+dimenStr[i],pb->elementBlockID());
         }
         else if(basis->getElementSpace()==panzer::PureBasis::HDIV) {
            for(int i=0;i<basis->dimension();i++)
               mesh->addCellField(fieldItr->first+dimenStr[i],pb->elementBlockID());
         }
         else if(basis->getElementSpace()==panzer::PureBasis::HVOL) {
            for(int i=0;i<basis->dimension();i++)
               mesh->addCellField(fieldItr->first+dimenStr[i],pb->elementBlockID());
         }
      }
   }
   mesh_factory->completeMeshConstruction(*mesh,MPI_COMM_WORLD);
   
   Teuchos::ParameterList pldiric("Dirichlet");
   {	
	   Teuchos::ParameterList& p0 = pldiric.sublist("a");  // noname sublist
	   p0.set("SideSet Name","left");
	   p0.set("Value Type","Constant");
       p0.set<Teuchos::Array<std::string> >("DOF Names",Teuchos::tuple<std::string>( "EFIELD" ));
	   Teuchos::ParameterList pl_sub("Constant");
	   pl_sub.set("Value",0.0);
	   p0.set("Constant",pl_sub);
	   
	   Teuchos::ParameterList& p1 = pldiric.sublist("b");  // noname sublist
	   p1.set("SideSet Name","top");
	   p1.set("Value Type","Constant");
       p1.set<Teuchos::Array<std::string> >("DOF Names",Teuchos::tuple<std::string>( "EFIELD" ));
	   p1.set("Constant",pl_sub);
	   
	   Teuchos::ParameterList& p2 = pldiric.sublist("c");  // noname sublist
	   p2.set("SideSet Name","right");
	   p2.set("Value Type","Constant");
       p2.set<Teuchos::Array<std::string> >("DOF Names",Teuchos::tuple<std::string>( "EFIELD" ));
	   p2.set("Constant",pl_sub);
	   
	   Teuchos::ParameterList& p3 = pldiric.sublist("d");  // noname sublist
	   p3.set("SideSet Name","bottom");
	   p3.set("Value Type","Constant");
       p3.set<Teuchos::Array<std::string> >("DOF Names",Teuchos::tuple<std::string>( "EFIELD" ));
	   p3.set("Constant",pl_sub);
   }


   // check that the bcs exist in the mesh
   /////////////////////////////////////////////////////////////
   {
     std::vector<std::string> eBlockNames;
     mesh->getElementBlockNames(eBlockNames);
     std::vector<std::string> sidesetNames;
     mesh->getSidesetNames(sidesetNames);
   }

   // build DOF Manager and linear object factory
   /////////////////////////////////////////////////////////////

   RCP<panzer::GlobalIndexer> dofManager;
   Teuchos::RCP<panzer::LinearObjFactory<panzer::Traits> > linObjFactory;

   // build the connection manager
   {
     const Teuchos::RCP<panzer::ConnManager> conn_manager = Teuchos::rcp(new panzer_stk::STKConnManager(mesh));

     panzer::DOFManagerFactory globalIndexerFactory;
     RCP<panzer::GlobalIndexer> dofManager_long
           = globalIndexerFactory.buildGlobalIndexer(Teuchos::opaqueWrapper(MPI_COMM_WORLD),physicsBlocks,conn_manager);
     dofManager = dofManager_long;

     // construct some linear algebra object, build object to pass to evaluators
     linObjFactory = Teuchos::rcp(new panzer::TpetraLinearObjFactory<panzer::Traits,double,int,panzer::GlobalOrdinal>(comm,dofManager_long));
   }

   // build worksets
   ////////////////////////////////////////////////////////

   Teuchos::RCP<panzer_stk::WorksetFactory> wkstFactory
      = Teuchos::rcp(new panzer_stk::WorksetFactory(mesh)); // build STK workset factory
   Teuchos::RCP<panzer::WorksetContainer> wkstContainer     // attach it to a workset container (uses lazy evaluation)
      = Teuchos::rcp(new panzer::WorksetContainer);
   wkstContainer->setFactory(wkstFactory);
   for(size_t i=0;i<physicsBlocks.size();i++)
     wkstContainer->setNeeds(physicsBlocks[i]->elementBlockID(),physicsBlocks[i]->getWorksetNeeds());
   wkstContainer->setWorksetSize(workset_size);
   wkstContainer->setGlobalIndexer(dofManager);

   // Setup response library for checking the error in this manufactered solution
   ////////////////////////////////////////////////////////////////////////

   Teuchos::RCP<panzer::ResponseLibrary<panzer::Traits> > errorResponseLibrary
      = Teuchos::rcp(new panzer::ResponseLibrary<panzer::Traits>(wkstContainer,dofManager,linObjFactory));

   {
     std::vector<std::string> eBlocks;
     mesh->getElementBlockNames(eBlocks);

     panzer::FunctionalResponse_Builder<int,int> builder;

     builder.comm = MPI_COMM_WORLD;
     builder.cubatureDegree = 10;
     builder.requiresCellIntegral = true;
     builder.quadPointField = "EFIELD_ERROR";

     errorResponseLibrary->addResponse("L2 Error",eBlocks,builder);

     builder.comm = MPI_COMM_WORLD;
     builder.cubatureDegree = 10;
     builder.requiresCellIntegral = true;
     builder.quadPointField = "EFIELD_HCURL_ERROR";

     errorResponseLibrary->addResponse("HCurl Error",eBlocks,builder);
   }

   // setup closure model
   /////////////////////////////////////////////////////////////

   // Add in the application specific closure model factory
   panzer::ClosureModelFactory_TemplateManager<panzer::Traits> cm_factory;
   Example::ClosureModelFactory_TemplateBuilder cm_builder;
   cm_factory.buildObjects(cm_builder);

   Teuchos::ParameterList closure_models("Closure Models");
   {
     closure_models.sublist("solid").sublist("SOURCE_EFIELD").set<std::string>("Type","SIMPLE SOURCE"); // a constant source
        // SOURCE_EFIELD field is required by the CurlLaplacianEquationSet

     // required for error calculation
     closure_models.sublist("solid").sublist("EFIELD_ERROR").set<std::string>("Type","L2 ERROR_CALC");
     closure_models.sublist("solid").sublist("EFIELD_ERROR").set<std::string>("Field A","EFIELD");
     closure_models.sublist("solid").sublist("EFIELD_ERROR").set<std::string>("Field B","EFIELD_EXACT");

     closure_models.sublist("solid").sublist("EFIELD_HCURL_ERROR").set<std::string>("Type","HCurl ERROR_CALC");
     closure_models.sublist("solid").sublist("EFIELD_HCURL_ERROR").set<std::string>("Field A","EFIELD");
     closure_models.sublist("solid").sublist("EFIELD_HCURL_ERROR").set<std::string>("Field B","EFIELD_EXACT");

     closure_models.sublist("solid").sublist("EFIELD_EXACT").set<std::string>("Type","EFIELD_EXACT");
     // closure_models.sublist("solid").sublist("EFIELD_EXACT").set<std::string>("Type","SIMPLE SOURCE");
   }

   Teuchos::ParameterList user_data("User Data"); // user data can be empty here

   // setup field manager builder
   /////////////////////////////////////////////////////////////

   Teuchos::RCP<panzer::FieldManagerBuilder> fmb =
         Teuchos::rcp(new panzer::FieldManagerBuilder);
   fmb->setWorksetContainer(wkstContainer);
   fmb->setupVolumeFieldManagers(physicsBlocks,cm_factory,closure_models,*linObjFactory,user_data);
   //fmb->setupBCFieldManagers(bcs,physicsBlocks,*eqset_factory,cm_factory,bc_factory,closure_models,
   //                          *linObjFactory,user_data);
   fmb->setupDiricheltFieldManagers(pldiric,mesh,dofManager);

   fmb->writeVolumeGraphvizDependencyFiles("volume",physicsBlocks);

   // setup assembly engine
   /////////////////////////////////////////////////////////////

   // build assembly engine: The key piece that brings together everything and
   //                        drives and controls the assembly process. Just add
   //                        matrices and vectors
   panzer::AssemblyEngine_TemplateManager<panzer::Traits> ae_tm;
   panzer::AssemblyEngine_TemplateBuilder builder(fmb,linObjFactory);
   ae_tm.buildObjects(builder);

   // Finalize construcition of STK writer response library
   /////////////////////////////////////////////////////////////
   {
      user_data.set<int>("Workset Size",workset_size);
      errorResponseLibrary->buildResponseEvaluators(physicsBlocks,
                                                    cm_factory,
                                                    closure_models,
                                                    user_data,true,"error");
   }

   // assemble linear system
   /////////////////////////////////////////////////////////////

   // build linear algebra objects: Ghost is for parallel assembly, it contains
   //                               local element contributions summed, the global IDs
   //                               are not unique. The non-ghosted or "global"
   //                               container will contain the sum over all processors
   //                               of the ghosted objects. The global indices are unique.
   RCP<panzer::LinearObjContainer> ghostCont = linObjFactory->buildGhostedLinearObjContainer();
   RCP<panzer::LinearObjContainer> container = linObjFactory->buildLinearObjContainer();
   linObjFactory->initializeGhostedContainer(panzer::LinearObjContainer::X |
                                             panzer::LinearObjContainer::F |
                                             panzer::LinearObjContainer::Mat,*ghostCont);
   linObjFactory->initializeContainer(panzer::LinearObjContainer::X |
                                      panzer::LinearObjContainer::F |
                                      panzer::LinearObjContainer::Mat,*container);
   ghostCont->initialize();
   container->initialize();

   // Actually evaluate
   /////////////////////////////////////////////////////////////

   panzer::AssemblyEngineInArgs input(ghostCont,container);
   input.alpha = 0;
   input.beta = 1;

   // evaluate physics: This does both the Jacobian and residual at once
   ae_tm.getAsObject<panzer::Traits::Jacobian>()->evaluate(input);

   // solve linear system
   /////////////////////////////////////////////////////////////
   solveTpetraSystem(*container);

   // output data (optional)
   /////////////////////////////////////////////////////////////

   // write out solution
   if(true) {
      // fill STK mesh objects


      // write to exodus
      // ---------------
      // Due to multiple instances of this test being run at the same
      // time (one for each order), we need to differentiate output to
      // prevent race conditions on output file. Multiple runs for the
      // same order are ok as they are staged one after another in the
      // ADD_ADVANCED_TEST cmake macro.
      std::ostringstream filename;
      filename << output_filename << basis_order << ".exo";
      mesh->writeToExodus(filename.str());
   }

   // compute error norm
   /////////////////////////////////////////////////////////////

   if(true) {
      Teuchos::FancyOStream lout(Teuchos::rcpFromRef(std::cout));
      lout.setOutputToRootOnly(0);

      panzer::AssemblyEngineInArgs respInput(ghostCont,container);
      respInput.alpha = 0;
      respInput.beta = 1;

      Teuchos::RCP<panzer::ResponseBase> l2_resp = errorResponseLibrary->getResponse<panzer::Traits::Residual>("L2 Error");
      Teuchos::RCP<panzer::Response_Functional<panzer::Traits::Residual> > l2_resp_func =
            Teuchos::rcp_dynamic_cast<panzer::Response_Functional<panzer::Traits::Residual> >(l2_resp);
      Teuchos::RCP<Thyra::VectorBase<double> > l2_respVec = Thyra::createMember(l2_resp_func->getVectorSpace());
      l2_resp_func->setVector(l2_respVec);

      Teuchos::RCP<panzer::ResponseBase> h1_resp = errorResponseLibrary->getResponse<panzer::Traits::Residual>("HCurl Error");
      Teuchos::RCP<panzer::Response_Functional<panzer::Traits::Residual> > h1_resp_func =
            Teuchos::rcp_dynamic_cast<panzer::Response_Functional<panzer::Traits::Residual> >(h1_resp);
      Teuchos::RCP<Thyra::VectorBase<double> > h1_respVec = Thyra::createMember(h1_resp_func->getVectorSpace());
      h1_resp_func->setVector(h1_respVec);

      errorResponseLibrary->addResponsesToInArgs<panzer::Traits::Residual>(respInput);
      errorResponseLibrary->evaluate<panzer::Traits::Residual>(respInput);

      lout << "L2 Error = " << sqrt(l2_resp_func->value) << std::endl;
      lout << "HCurl Error = " << sqrt(h1_resp_func->value) << std::endl;
   }

   stackedTimer->stop("Curl Laplacian");
   Teuchos::StackedTimer::OutputOptions options;
   options.output_fraction = true;
   options.output_minmax = true;
   options.output_histogram = true;
   options.num_histogram = 5;
   stackedTimer->report(std::cout, Teuchos::DefaultComm<int>::getComm(), options);

   // all done!
   /////////////////////////////////////////////////////////////
   out << "ALL PASSED: Tpetra" << std::endl;
   return 0;
}

void solveTpetraSystem(panzer::LinearObjContainer & container)
{
  typedef panzer::TpetraLinearObjContainer<double,int,panzer::GlobalOrdinal> LOC;

  LOC & tp_container = Teuchos::dyn_cast<LOC>(container);

  // do stuff
  // Wrap the linear problem to solve in a Belos::LinearProblem
  // object.  The "X" argument of the LinearProblem constructor is
  // only copied shallowly and will be overwritten by the solve, so we
  // make a deep copy here.  That way we can compare the result
  // against the original X_guess.
  typedef Tpetra::MultiVector<double,int,panzer::GlobalOrdinal> MV;
  typedef Tpetra::Operator<double,int,panzer::GlobalOrdinal> OP;
  typedef Belos::LinearProblem<double,MV, OP> ProblemType;
  Teuchos::RCP<ProblemType> problem(new ProblemType(tp_container.get_A(), tp_container.get_x(), tp_container.get_f()));
  TEUCHOS_ASSERT(problem->setProblem());

  typedef Belos::PseudoBlockGmresSolMgr<double,MV,OP> SolverType;

  Teuchos::ParameterList belosList;
  belosList.set( "Num Blocks", 3000 );            // Maximum number of blocks in Krylov factorization
  belosList.set( "Block Size", 1 );              // Blocksize to be used by iterative solver
  belosList.set( "Maximum Iterations", 50000 );       // Maximum number of iterations allowed
  belosList.set( "Maximum Restarts", 20 );      // Maximum number of restarts allowed
  belosList.set( "Convergence Tolerance", 1e-9 );         // Relative convergence tolerance requested
  belosList.set( "Verbosity", Belos::Errors + Belos::Warnings + Belos::TimingDetails + Belos::StatusTestDetails );
  belosList.set( "Output Frequency", 1 );
  belosList.set( "Output Style", 1 );

  SolverType solver(problem, Teuchos::rcpFromRef(belosList));

  Belos::ReturnType result = solver.solve();
  if (result == Belos::Converged)
    std::cout << "Result: Converged." << std::endl;
  else {
    TEUCHOS_ASSERT(false); // FAILURE!
  }

  // scale by -1
  tp_container.get_x()->scale(-1.0);

  tp_container.get_A()->resumeFill(); // where does this go?
}
