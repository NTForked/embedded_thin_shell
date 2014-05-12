#include <eigen3/Eigen/Dense>
#include <JsonFilePaser.h>
#include "ReducedSimCubature.h"

using namespace Eigen;
using namespace UTILITY;
using namespace CUBATURE;

/**
 * Required input (using a json file):
 * 
 * B: basis matrix, generated by model derivative or by extending MA basis.
 * tet_mesh: tetrahedron mesh.
 * training_full_disp: displacements in full space, e.g. u[i].
 * training_full_forces: forces in full sapce, e.g. f[i].
 * rel_err_tol: relative error tolerance, usually set as 0.05.
 * max_points: desired number of samples, usually set as 80.
 * cands_per_iter: usually set as 400.
 * iters_per_full_nnls: usually set as 10.
 * samples_per_subtrain: usually set as 100.
 * 
 * @see 1. An efficient construction of reduced deformable objects, asia 2013.
 *      2. Optimizing Cubature for Efficient Integration of Subspace Deformations. asia 2008.
 * 
 */
int main(int argc, char *argv[]){

  // check
  if (argc < 2){
	ERROR_LOG("ussage: ./cubature json_file_name");
	return -1;
  }
  JsonFilePaser jsonf;
  if(!jsonf.open(argv[1])){
	ERROR_LOG("failed to open json file: " << argv[1]);
	return -1;
  }

  // load basis
  INFO_LOG("loading data....");
  MatrixXd B;
  jsonf.readMatFile("subspace_basis",B);

  // load tetrahedron mesh, and elastic material
  pTetMesh tet_mesh = pTetMesh(new TetMesh());
  string tet_file, elastic_mtl;
  if(jsonf.readFilePath("vol_file", tet_file)){
	if(!tet_mesh->load(tet_file)){
	  ERROR_LOG("failed to load tet mesh file: " << tet_file);
	  return -1;
	}else if(jsonf.readFilePath("elastic_mtl", elastic_mtl)){
	  const bool s = tet_mesh->loadElasticMtl(elastic_mtl);
	  ERROR_LOG_COND("failed to load the elastic material from: "<<elastic_mtl,s);
	}
  }
  assert_eq(B.rows(), tet_mesh->nodes().size()*3);

  // load training data
  MatrixXd training_full_disp, training_full_forces;
  jsonf.readMatFile("training_full_u", training_full_disp);
  jsonf.readMatFile("training_full_f", training_full_forces);

  // load other parameters
  double rel_err_tol;
  int max_points, cands_per_iter, iters_per_full_nnls, samples_per_subtrain;
  jsonf.read("rel_err_tol", rel_err_tol,0.05);
  jsonf.read("max_samples", max_points,80);
  jsonf.read("cands_per_iter", cands_per_iter,400);
  jsonf.read("iters_per_full_nnls", iters_per_full_nnls,10);
  jsonf.read("samples_per_subtrain", samples_per_subtrain,100);

  // run cubature
  INFO_LOG("running cubature....");
  ReducedSimCubature cuba(B, tet_mesh);
  cuba.run(training_full_disp, 
  		   training_full_forces, 
  		   rel_err_tol, 
  		   max_points, 
  		   cands_per_iter, 
  		   iters_per_full_nnls, 
  		   samples_per_subtrain);

  // save cubature
  cout<< "cubature weights:\n " << cuba.getWeights().transpose() << endl << endl;;
  string save_points;
  jsonf.readFilePath("cubature_points", save_points,string("cubpoints.txt"),false);
  INFO_LOG("save samples to: "<< save_points);
  UTILITY::writeVec(save_points,cuba.getSelectedTets(),UTILITY::TEXT);

  string save_weights;
  jsonf.readFilePath("cubature_weights", save_weights,string("cubweights.b"),false);
  INFO_LOG("save weights to: "<< save_weights);
  UTILITY::writeVec(save_weights,cuba.getWeights());

  bool succ = cuba.saveAsVTK(save_points+".vtk");
  ERROR_LOG_COND("failed to save as vtk: "<< save_points+".vtk",succ);
  
  return 0;
}
