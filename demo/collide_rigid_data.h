#ifndef _COLLIDE_RIGID_DATA_H_
#define _COLLIDE_RIGID_DATA_H_

#include <iostream>
#include <vector>
#include <Eigen/Dense>
#include <boost/shared_ptr.hpp>

#include <Objmesh.h>
#include <TetMesh.h>

namespace COLIDE_RIGID {

  class Plane {
  public:
    void Collide (const UTILITY::VVec3d &nodes, const double kd, Eigen::VectorXd &u, Eigen::VectorXd &v);
    friend ostream& operator<<(ostream&, Plane&);
    Eigen::Vector3d &GetPoint () { return point_; }
    Eigen::Vector3d &GetNormal () { return normal_; }
  private:
    Eigen::Vector3d point_;
    Eigen::Vector3d normal_;
  };
  typedef boost::shared_ptr<Plane> pPlane;

  class RigidBall {
  public:
    void Collide (const UTILITY::VVec3d &nodes,  const double k, const Eigen::VectorXd &u, Eigen::VectorXd &extforce) const;
    int ExportObj (const string &filename) const;
    bool InitFromObj (const string &filename);
    void Transform (const Vector3d &dis);
    Vector3d CalGravity (const Vector3d &g_normal);
  private:
    void CalSetCR (); // caculate and set center and radius
    double r_;
    double density_;
    Eigen::Vector3d center_;
    UTILITY::Objmesh obj_;
  };
  typedef boost::shared_ptr<RigidBall> pRigidBall;

  class SenceData {
  public:
    int InitDataFromFile (const char *ini_file);

    int steps_;
    int output_steps_;
    double time_step_;
    double kd_; // kd for velocity
    double k_; // stiffness k for collision force
    Eigen::Vector3d g_normal_; // gravity normal
    string ini_file_; // init file path
    string out_ball_prefix_;
    string out_tet_mesh_prefix_;
    string out_shell_mesh_prefix_;

    Eigen::MatrixXd tet_mesh_gravity_;
    UTILITY::pTetMesh tet_mesh_;

    RigidBall rigid_ball_;
    std::vector<pPlane> planes_;
  private:
    int LoadData (const char *ini_file);
    int CalTetMeshGravity ();
  };
  typedef boost::shared_ptr<SenceData> pSenceData;

  ostream& operator<<(ostream&, Plane&);
}

#endif /* _COLIDE_RIGID_DATA_H_ */
