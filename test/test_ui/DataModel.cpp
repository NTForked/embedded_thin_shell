#include "DataModel.h"
#include <SubspaceSimulator.h>
#include <FullStVKSimulator.h>
#include <MatrixIO.h>
#include <jtflib/mesh/util.h>
#include "interpolator/interpolator.h"
#include "shell_deformer/cluster.h"
#include "conf_para.h"

using namespace SIMULATOR;

DataModel::DataModel(pTetMeshEmbeding embeding):_volObj(embeding){

    assert(embeding);
    _simulator = pSimulator(new FullStVKSimulator());
}

pSimulator DataModel::createSimulator(const string filename)const{

    string simulator_name = "full_stvk";
    JsonFilePaser jsonf;
    if (jsonf.open(filename)){
        jsonf.read("simulator", simulator_name, string("full_stvk"));
    }

    pSimulator sim;
    if ("subspace" == simulator_name){
        pReducedElasticModel elas_m = pReducedElasticModel(new DirectReductionElasticModel());
        sim = pSimulator(new SubspaceSimulator(elas_m,string("subspace")));
    }else if ("cubature" == simulator_name){
        pReducedElasticModel elas_m = pReducedElasticModel(new CubaturedElasticModel());
        sim = pSimulator(new SubspaceSimulator(elas_m,string("cubature")));
    }else{
        sim = pSimulator(new FullStVKSimulator());
    }

    return sim;
}

bool DataModel::loadSetting(const string filename){

    JsonFilePaser jsonf;
    if (!jsonf.open(filename)){
        ERROR_LOG("failed to open: " << filename);
        return false;
    }

    _simulator = createSimulator(filename);

    bool succ = true;
    string mtlfile;
    if (jsonf.readFilePath("elastic_mtl",mtlfile)){
        if (_volObj && _volObj->getTetMesh()){
            succ = _volObj->getTetMesh()->loadElasticMtl(mtlfile);
        }
    }

    // string fixed_node_file;
    // if (jsonf.readFilePath("fixed_nodes", fixed_node_file)){
    // 	succ &= loadFixedNodes(fixed_node_file);
    // }

    if (_simulator){
        succ &= _simulator->init(filename);
    }

    print();
    return succ;
}

void DataModel::prepareSimulation(){

    if(_simulator){
        _simulator->setVolMesh(_volObj->getTetMesh());
        const bool succ = _simulator->precompute();
        ERROR_LOG_COND("the precomputation is failed.",succ);


        ///add my modification *******************************************8
        ///
        {
            std::vector<size_t> buffst;
            std::vector<double> buffd;
            _volObj->getTetMesh()->tets(buffst);
            _volObj->getTetMesh()->nodes(buffd);
            tet_mesh_.resize(4, buffst.size() / 4);
            tet_nodes_.resize(3, buffd.size() / 3);
            std::copy(buffst.begin(), buffst.end(), tet_mesh_.begin());
            std::copy(buffd.begin(), buffd.end(), tet_nodes_.begin());
            gen_outside_shell(tet_mesh_, tet_nodes_, shell_mesh_, shell_nodes_, shell_normal_,
                              __SUBDIVISION_TIME, __EMBED_DEPTH);
        }
        {
            size_t row = tet_nodes_.size(2);
            size_t col = shell_nodes_.size(2);
            B_.resize(row, col);
            hj::sparse::spm_csc<double> B(row, col);
            tet_embed(tet_nodes_, tet_mesh_, shell_nodes_, B);
            for (size_t j = 0; j < col; ++j)
                for (size_t i = 0; i < row; ++i)
                    B_(i, j) = B(i, j);
        }
        {
            cluster_machine handle(shell_mesh_, shell_nodes_, __CLUSTER_RADIUS);
            handle.partition(__REGION_COUNT);
            regions_ = handle.regions_;
        }
        shell_deformer_ = shared_ptr<deformer>(new deformer(shell_mesh_, shell_nodes_,
                                                            shell_nodes_, regions_));
        dx_.resize(tet_nodes_.size(1), tet_nodes_.size(2));
        q_.resize(tet_nodes_.size(1), tet_nodes_.size(2));
        xq_.resize(shell_nodes_.size(1), shell_nodes_.size(2));
        ///end my modification**************************************************

    }
}

void DataModel::setForces(const int nodeId,const double force[3]){

    if(_simulator){
        _simulator->setExtForceOfNode(nodeId,force);
        this->simulate();
    }
}

void DataModel::getSubUc(const vector<set<int> > &groups,const VectorXd &full_u,Matrix<double,3,-1> &sub_u)const{

    int nodes = 0;
    BOOST_FOREACH(const set<int>& s, groups)
            nodes += s.size();

    sub_u.resize(3,nodes);
    int index = 0;
    BOOST_FOREACH(const set<int>& s, groups){
        BOOST_FOREACH(const int i, s){
            assert_in(i*3,0,full_u.size()-3);
            sub_u.col(index) = full_u.segment<3>(i*3);
            index++;
        }
    }
}

void DataModel::updateUc(const Matrix<double,3,-1> &uc,const int group_id){

    _partialCon.updatePc(uc,group_id);
    if(uc.size() > 0){
        const int n = _partialCon.getPc().size();
        _simulator->setUc(Map<VectorXd>(const_cast<double*>(&(_partialCon.getPc()(0,0))),n));
    }
}

void DataModel::resetPartialCon(){

    Matrix<double,3,-1> pc;
    getSubUc(_partialCon.getConNodesSet(),getU(),pc);
    _partialCon.updatePc(pc);
    static vector<int> con_nodes;
    static VectorXd con_uc;
    _partialCon.getPartialCon(con_nodes, con_uc);
    _simulator->setConNodes(con_nodes);
    _simulator->setUc(con_uc);
}

bool DataModel::simulate(){

    bool succ = false;
    /**
     * @brief elastic solid component
     */
    if(_simulator){
        succ = _simulator->forward();
        //	_volObj->interpolate(getU());

    }
    /**
     * @brief shell deformation component
     */
//    {
//        VectorXd disp = getU();
//        std::copy(disp.data(), disp.data() + disp.size(), dx_.begin());
//        q_ = tet_nodes_ + dx_;

//        xq_ = q_ * B_;
//        shell_deformer_->deform(shell_nodes_, xq_);
//        jtf::mesh::cal_point_normal(shell_mesh_, shell_nodes_, shell_normal_);

//    }

    return succ;
}
