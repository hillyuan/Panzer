// @HEADER
// ***********************************************************************
//
//           Panzer: A partial differential equation assembly
//       engine for strongly coupled complex multiphysics systems
//                 Copyright (2011) Sandia Corporation
//
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
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
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact Roger P. Pawlowski (rppawlo@sandia.gov) and
// Eric C. Cyr (eccyr@sandia.gov)
// ***********************************************************************
// @HEADER

#include "Panzer_LocalPartitioningUtilities.hpp"

#include "Teuchos_RCP.hpp"
#include "Teuchos_Comm.hpp"
#include "Teuchos_Assert.hpp"

#include "Tpetra_CrsMatrix.hpp"
#include "Tpetra_RowMatrixTransposer.hpp"

#include "Panzer_FaceToElement.hpp"
#include "Panzer_ConnManager.hpp"
#include "Panzer_NodeType.hpp"
#include "Panzer_FieldPattern.hpp"
#include "Panzer_NodalFieldPattern.hpp"
#include "Panzer_EdgeFieldPattern.hpp"
#include "Panzer_FaceFieldPattern.hpp"
#include "Panzer_ElemFieldPattern.hpp"

#include "Panzer_Workset_Builder.hpp"
#include "Panzer_WorksetDescriptor.hpp"

#include "Phalanx_KokkosDeviceTypes.hpp"

#include <set>
#include <unordered_set>
#include <unordered_map>

namespace panzer
{

namespace partitioning_utilities
{


void
setupSubLocalMeshInfo(const panzer::LocalMeshInfoBase & parent_info,
                      const std::vector<panzer::LocalOrdinal> & owned_parent_cells,
                      panzer::LocalMeshInfoBase & sub_info)
{
  using GO = panzer::GlobalOrdinal;
  using LO = panzer::LocalOrdinal;

  PANZER_FUNC_TIME_MONITOR_DIFF("panzer::partitioning_utilities::setupSubLocalMeshInfo",setupSLMI);
  // The goal of this function is to fill a LocalMeshInfoBase (sub_info) with
  // a subset of cells from a given parent LocalMeshInfoBase (parent_info)

  // Note: owned_parent_cells are the owned cells for sub_info in the parent_info's indexing scheme
  // We need to generate sub_info's ghosts and figure out the virtual cells

  // Note: We will only handle a single ghost layer

  // Note: We assume owned_parent_cells are owned cells of the parent
  // i.e. owned_parent_indexes cannot refer to ghost or virtual cells in parent_info

  // Note: This function works with inter-face connectivity. NOT node connectivity.

  const int num_owned_cells = owned_parent_cells.size();
  TEUCHOS_TEST_FOR_EXCEPT_MSG(num_owned_cells == 0, "panzer::partitioning_utilities::setupSubLocalMeshInfo : Input parent subcells must exist (owned_parent_cells)");

  const int num_parent_owned_cells = parent_info.num_owned_cells;
  TEUCHOS_TEST_FOR_EXCEPT_MSG(num_parent_owned_cells <= 0, "panzer::partitioning_utilities::setupSubLocalMeshInfo : Input parent info must contain owned cells");

  const int num_parent_ghstd_cells = parent_info.num_ghstd_cells;

  const int num_parent_total_cells = parent_info.num_owned_cells + parent_info.num_ghstd_cells + parent_info.num_virtual_cells;

  // Just as a precaution, make sure the parent_info is setup properly
  TEUCHOS_ASSERT(static_cast<int>(parent_info.cell_to_faces.extent(0)) == num_parent_total_cells);
  const int num_faces_per_cell = parent_info.cell_to_faces.extent(1);

  // The first thing to do is construct a vector containing the parent cell indexes of all
  // owned, ghstd, and virtual cells
  std::vector<LO> ghstd_parent_cells;
  std::vector<LO> virtual_parent_cells;
  {
    PANZER_FUNC_TIME_MONITOR_DIFF("Construct parent cell vector",ParentCell);
    // We grab all of the owned cells and put their global indexes into sub_info
    // We also put all of the owned cell indexes in the parent's indexing scheme into a set to use for lookups
    std::unordered_set<LO> owned_parent_cells_set(owned_parent_cells.begin(), owned_parent_cells.end());

    // We need to create a list of ghstd and virtual cells
    // We do this by running through sub_cell_indexes
    // and looking at the neighbors to find neighbors that are not owned

    // Virtual cells are defined as cells with indexes outside of the range of owned_cells and ghstd_cells
    const int virtual_parent_cell_offset = num_parent_owned_cells + num_parent_ghstd_cells;

    std::unordered_set<LO> ghstd_parent_cells_set;
    std::unordered_set<LO> virtual_parent_cells_set;
    auto cell_to_faces_h = Kokkos::create_mirror_view(parent_info.cell_to_faces);
    auto face_to_cells_h = Kokkos::create_mirror_view(parent_info.face_to_cells);
    Kokkos::deep_copy(cell_to_faces_h, parent_info.cell_to_faces);
    Kokkos::deep_copy(face_to_cells_h, parent_info.face_to_cells);
    for(int i=0;i<num_owned_cells;++i){
      const LO parent_cell_index = owned_parent_cells[i];
      for(int local_face_index=0;local_face_index<num_faces_per_cell;++local_face_index){
        const LO parent_face = cell_to_faces_h(parent_cell_index, local_face_index);

        // Sidesets can have owned cells that border the edge of the domain (i.e. parent_face == -1)
        // If we are at the edge of the domain, we can ignore this face.
        if(parent_face < 0)
          continue;

        // Find the side index for neighbor cell with respect to the face
        const LO neighbor_parent_side = (face_to_cells_h(parent_face,0) == parent_cell_index) ? 1 : 0;

        // Get the neighbor cell index in the parent's indexing scheme
        const LO neighbor_parent_cell = face_to_cells_h(parent_face, neighbor_parent_side);

        // If the face exists, then the neighbor should exist
        TEUCHOS_ASSERT(neighbor_parent_cell >= 0);

        // We can easily check if this is a virtual cell
        if(neighbor_parent_cell >= virtual_parent_cell_offset){
          virtual_parent_cells_set.insert(neighbor_parent_cell);
        } else if(neighbor_parent_cell >= num_parent_owned_cells){
          // This is a quick check for a ghost cell
          // This branch only exists to cut down on the number of times the next branch (much slower) is called
          ghstd_parent_cells_set.insert(neighbor_parent_cell);
        } else {
          // There is still potential for this to be a ghost cell with respect to 'our' cells
          // The only way to check this is with a super slow lookup call
          if(owned_parent_cells_set.find(neighbor_parent_cell) == owned_parent_cells_set.end()){
            // The neighbor cell is not owned by 'us', therefore it is a ghost
            ghstd_parent_cells_set.insert(neighbor_parent_cell);
          }
        }
      }
    }

    // We now have a list of the owned, ghstd, and virtual cells in the parent's indexing scheme.
    // We will take the 'unordered_set's ordering for the the sub-indexing scheme

    ghstd_parent_cells.assign(ghstd_parent_cells_set.begin(), ghstd_parent_cells_set.end());
    virtual_parent_cells.assign(virtual_parent_cells_set.begin(), virtual_parent_cells_set.end());

  }

  const int num_ghstd_cells = ghstd_parent_cells.size();
  const int num_virtual_cells = virtual_parent_cells.size();
  const int num_real_cells = num_owned_cells + num_ghstd_cells;
  const int num_total_cells = num_real_cells + num_virtual_cells;

  std::vector<std::pair<LO, LO> > all_parent_cells(num_total_cells);
  for (std::size_t i=0; i< owned_parent_cells.size(); ++i)
    all_parent_cells[i] = std::pair<LO, LO>(owned_parent_cells[i], i);

  for (std::size_t i=0; i< ghstd_parent_cells.size(); ++i) {
    LO insert = owned_parent_cells.size()+i;
    all_parent_cells[insert] = std::pair<LO, LO>(ghstd_parent_cells[i], insert);
  }

  for (std::size_t i=0; i< virtual_parent_cells.size(); ++i) {
    LO insert = owned_parent_cells.size()+ ghstd_parent_cells.size()+i;
    all_parent_cells[insert] = std::pair<LO, LO>(virtual_parent_cells[i], insert);
  }

  sub_info.num_owned_cells = owned_parent_cells.size();
  sub_info.num_ghstd_cells = ghstd_parent_cells.size();
  sub_info.num_virtual_cells = virtual_parent_cells.size();

  // We now have the indexing order for our sub_info

  // Just as a precaution, make sure the parent_info is setup properly
  TEUCHOS_ASSERT(static_cast<int>(parent_info.cell_vertices.extent(0)) == num_parent_total_cells);
  TEUCHOS_ASSERT(static_cast<int>(parent_info.local_cells.extent(0)) == num_parent_total_cells);
  TEUCHOS_ASSERT(static_cast<int>(parent_info.global_cells.extent(0)) == num_parent_total_cells);

  const int num_vertices_per_cell = parent_info.cell_vertices.extent(1);
  const int num_dims = parent_info.cell_vertices.extent(2);

  // Fill owned, ghstd, and virtual cells: global indexes, local indexes and vertices
  sub_info.global_cells = PHX::View<GO*>("global_cells", num_total_cells);
  sub_info.local_cells = PHX::View<LO*>("local_cells", num_total_cells);
  sub_info.cell_vertices = PHX::View<double***>("cell_vertices", num_total_cells, num_vertices_per_cell, num_dims);
  auto global_cells_h =  Kokkos::create_mirror_view(sub_info.global_cells);
  auto local_cells_h =   Kokkos::create_mirror_view(sub_info.local_cells);
  auto cell_vertices_h = Kokkos::create_mirror_view(sub_info.cell_vertices);
  auto p_global_cells_h =  Kokkos::create_mirror_view(parent_info.global_cells);
  auto p_local_cells_h =   Kokkos::create_mirror_view(parent_info.local_cells);
  auto p_cell_vertices_h = Kokkos::create_mirror_view(parent_info.cell_vertices);
  Kokkos::deep_copy(p_global_cells_h,parent_info.global_cells);
  Kokkos::deep_copy(p_local_cells_h,parent_info.local_cells);
  Kokkos::deep_copy(p_cell_vertices_h,parent_info.cell_vertices);

  for (int cell=0; cell<num_total_cells; ++cell) {
    const LO parent_cell = all_parent_cells[cell].first;
    global_cells_h(cell) = p_global_cells_h(parent_cell);
    local_cells_h(cell) = p_local_cells_h(parent_cell);
    for(int vertex=0;vertex<num_vertices_per_cell;++vertex){
      for(int dim=0;dim<num_dims;++dim){
        cell_vertices_h(cell,vertex,dim) = p_cell_vertices_h(parent_cell,vertex,dim);
      }
    }
  }
  Kokkos::deep_copy(sub_info.global_cells, global_cells_h);
  Kokkos::deep_copy(sub_info.local_cells, local_cells_h);
  Kokkos::deep_copy(sub_info.cell_vertices, cell_vertices_h);
  // Now for the difficult part

  // We need to create a new face indexing scheme from the old face indexing scheme

  // Create an auxiliary list with all cells - note this preserves indexing

  struct face_t{
    face_t(LO c0, LO c1, LO sc0, LO sc1)
    {
      cell_0=c0;
      cell_1=c1;
      subcell_index_0=sc0;
      subcell_index_1=sc1;
    }
    LO cell_0;
    LO cell_1;
    LO subcell_index_0;
    LO subcell_index_1;
  };


  // First create the faces
  std::vector<face_t> faces;
  {
    PANZER_FUNC_TIME_MONITOR_DIFF("Create faces",CreateFaces);
    // faces_set: cell_0, subcell_index_0, cell_1, subcell_index_1
    std::unordered_map<LO,std::unordered_map<LO, std::pair<LO,LO> > > faces_set;

    std::sort(all_parent_cells.begin(), all_parent_cells.end());

    auto cell_to_faces_h = Kokkos::create_mirror_view(parent_info.cell_to_faces);
    auto face_to_cells_h = Kokkos::create_mirror_view(parent_info.face_to_cells);
    auto face_to_lidx_h = Kokkos::create_mirror_view(parent_info.face_to_lidx);
    Kokkos::deep_copy(cell_to_faces_h, parent_info.cell_to_faces);
    Kokkos::deep_copy(face_to_cells_h, parent_info.face_to_cells);
    Kokkos::deep_copy(face_to_lidx_h, parent_info.face_to_lidx);
    for(int owned_cell=0;owned_cell<num_owned_cells;++owned_cell){
      const LO owned_parent_cell = owned_parent_cells[owned_cell];
      for(int local_face=0;local_face<num_faces_per_cell;++local_face){
        const LO parent_face = cell_to_faces_h(owned_parent_cell,local_face);

        // Skip faces at the edge of the domain
        if(parent_face<0)
          continue;

        // Get the cell on the other side of the face
        const LO neighbor_side = (face_to_cells_h(parent_face,0) == owned_parent_cell) ? 1 : 0;

        const LO neighbor_parent_cell = face_to_cells_h(parent_face, neighbor_side);
        const LO neighbor_subcell_index = face_to_lidx_h(parent_face, neighbor_side);

        // Convert parent cell index into sub cell index
        std::pair<LO, LO> search_point(neighbor_parent_cell, 0);
        auto itr = std::lower_bound(all_parent_cells.begin(), all_parent_cells.end(), search_point);

        TEUCHOS_TEST_FOR_EXCEPT_MSG(itr == all_parent_cells.end(), "panzer_stk::setupSubLocalMeshInfo : Neighbor cell was not found in owned, ghosted, or virtual cells");

        const LO neighbor_cell = itr->second;

        LO cell_0, cell_1, subcell_index_0, subcell_index_1;
        if(owned_cell < neighbor_cell){
          cell_0 = owned_cell;
          subcell_index_0 = local_face;
          cell_1 = neighbor_cell;
          subcell_index_1 = neighbor_subcell_index;
        } else {
          cell_1 = owned_cell;
          subcell_index_1 = local_face;
          cell_0 = neighbor_cell;
          subcell_index_0 = neighbor_subcell_index;
        }

        // Add this interface to the set of faces - smaller cell index is 'left' (or '0') side of face
        faces_set[cell_0][subcell_index_0] = std::pair<LO,LO>(cell_1, subcell_index_1);
      }
    }

    for(const auto & cell_pair : faces_set){
      const LO cell_0 = cell_pair.first;
      for(const auto & subcell_pair : cell_pair.second){
        const LO subcell_index_0 = subcell_pair.first;
        const LO cell_1 = subcell_pair.second.first;
        const LO subcell_index_1 = subcell_pair.second.second;
        faces.push_back(face_t(cell_0,cell_1,subcell_index_0,subcell_index_1));
      }
    }
  }

  const int num_faces = faces.size();

  sub_info.face_to_cells = PHX::View<LO*[2]>("face_to_cells", num_faces);
  sub_info.face_to_lidx = PHX::View<LO*[2]>("face_to_lidx", num_faces);
  sub_info.cell_to_faces = PHX::View<LO**>("cell_to_faces", num_total_cells, num_faces_per_cell);
  auto cell_to_faces_h = Kokkos::create_mirror_view(sub_info.cell_to_faces);
  auto face_to_cells_h = Kokkos::create_mirror_view(sub_info.face_to_cells);
  auto face_to_lidx_h = Kokkos::create_mirror_view(sub_info.face_to_lidx);


  // Default the system with invalid cell index
  Kokkos::deep_copy(cell_to_faces_h, -1);

  for(int face_index=0;face_index<num_faces;++face_index){
    const face_t & face = faces[face_index];

    face_to_cells_h(face_index,0) = face.cell_0;
    face_to_cells_h(face_index,1) = face.cell_1;

    cell_to_faces_h(face.cell_0,face.subcell_index_0) = face_index;
    cell_to_faces_h(face.cell_1,face.subcell_index_1) = face_index;

    face_to_lidx_h(face_index,0) = face.subcell_index_0;
    face_to_lidx_h(face_index,1) = face.subcell_index_1;

  }
  Kokkos::deep_copy(sub_info.cell_to_faces, cell_to_faces_h);
  Kokkos::deep_copy(sub_info.face_to_cells, face_to_cells_h);
  Kokkos::deep_copy(sub_info.face_to_lidx,  face_to_lidx_h);
  // Complete.

}

void
splitMeshInfo(const panzer::LocalMeshInfoBase & mesh_info,
              const int splitting_size,
              std::vector<panzer::LocalMeshPartition> & partitions)
{

  using LO = panzer::LocalOrdinal;

  // Make sure the splitting size makes sense
  TEUCHOS_ASSERT((splitting_size > 0) or (splitting_size == WorksetSizeType::ALL_ELEMENTS));

  // Default partition size
  const LO base_partition_size = std::min(mesh_info.num_owned_cells, (splitting_size > 0) ? splitting_size : mesh_info.num_owned_cells);

  // Cells to partition
  std::vector<LO> partition_cells;
  partition_cells.resize(base_partition_size);

  // Create the partitions
  LO cell_count = 0;
  while(cell_count < mesh_info.num_owned_cells){

    LO partition_size = base_partition_size;
    if(cell_count + partition_size > mesh_info.num_owned_cells)
      partition_size = mesh_info.num_owned_cells - cell_count;

    // Error check for a null partition - this should never happen by design
    TEUCHOS_ASSERT(partition_size != 0);

    // In the final partition, we need to reduce the size of partition_cells
    if(partition_size != base_partition_size)
      partition_cells.resize(partition_size);

    // Set the partition indexes - not really a partition, just a chunk of cells
    for(LO i=0; i<partition_size; ++i)
      partition_cells[i] = cell_count+i;

    // Create an empty partition
    partitions.push_back(panzer::LocalMeshPartition());

    // Fill the empty partition
    partitioning_utilities::setupSubLocalMeshInfo(mesh_info,partition_cells,partitions.back());

    // Update the cell count
    cell_count += partition_size;
  }

}

}

void
generateLocalMeshPartitions(const panzer::LocalMeshInfo & mesh_info,
                            const panzer::WorksetDescriptor & description,
                            std::vector<panzer::LocalMeshPartition> & partitions)
{
  // We have to make sure that the partitioning is possible
  TEUCHOS_ASSERT(description.getWorksetSize() != panzer::WorksetSizeType::CLASSIC_MODE);
  TEUCHOS_ASSERT(description.getWorksetSize() != 0);

  // This could just return, but it would be difficult to debug why no partitions were returned
  TEUCHOS_ASSERT(description.requiresPartitioning());

  const std::string & element_block_name = description.getElementBlock();

  // We have two processes for in case this is a sideset or element block
  if(description.useSideset()){

    // If the element block doesn't exist, there are no partitions to create
    if(mesh_info.sidesets.find(element_block_name) == mesh_info.sidesets.end())
      return;
    const auto & sideset_map = mesh_info.sidesets.at(element_block_name);

    const std::string & sideset_name = description.getSideset();

    // If the sideset doesn't exist, there are no partitions to create
    if(sideset_map.find(sideset_name) == sideset_map.end())
      return;

    const panzer::LocalMeshSidesetInfo & sideset_info = sideset_map.at(sideset_name);

    // Partitioning is not important for sidesets
    panzer::partitioning_utilities::splitMeshInfo(sideset_info, description.getWorksetSize(), partitions);

    for(auto & partition : partitions){
      partition.sideset_name = sideset_name;
      partition.element_block_name = element_block_name;
      partition.cell_topology = sideset_info.cell_topology;
      partition.has_connectivity = true;
    }

  } else {

    // If the element block doesn't exist, there are no partitions to create
    if(mesh_info.element_blocks.find(element_block_name) == mesh_info.element_blocks.end())
      return;

    // Grab the element block we're interested in
    const panzer::LocalMeshBlockInfo & block_info = mesh_info.element_blocks.at(element_block_name);

    if(description.getWorksetSize() == panzer::WorksetSizeType::ALL_ELEMENTS){
      // We only have one partition describing the entire local mesh
      panzer::partitioning_utilities::splitMeshInfo(block_info, -1, partitions);
    } else {
      // We need to partition local mesh

      // FIXME: Until the above function is fixed, we will use this hack - this will lead to horrible partitions
      panzer::partitioning_utilities::splitMeshInfo(block_info, description.getWorksetSize(), partitions);

    }

    for(auto & partition : partitions){
      partition.element_block_name = element_block_name;
      partition.cell_topology = block_info.cell_topology;
      partition.has_connectivity = true;
    }
  }

}


void
fillLocalCellIDs(const Teuchos::RCP<const Teuchos::Comm<int>> & comm,
                 Teuchos::RCP<panzer::ConnManager> & conn,
                 PHX::View<panzer::GlobalOrdinal*> & owned_cells,
                 PHX::View<panzer::GlobalOrdinal*> & ghost_cells,
                 PHX::View<panzer::GlobalOrdinal*> & virtual_cells)
{
  // Build the local to global cell ID map
  owned_cells = conn->getOwnedGlobalCellID();
  // Get ghost cells
  ghost_cells = conn -> getGhostGlobalCellID();

  // Build virtual cells
  // Note: virtual cells are currently defined by faces (only really used for FV/DG type discretizations)

  // this class comes from Mini-PIC and Matt B
  auto faceToElement = Teuchos::rcp(new panzer::FaceToElement<panzer::LocalOrdinal,panzer::GlobalOrdinal>());
  faceToElement->initialize(conn);
  auto elems_by_face = faceToElement->getFaceToElementsMap();
  auto face_to_lidx  = faceToElement->getFaceToCellLocalIdxMap();

  const panzer::LocalOrdinal num_owned_cells = owned_cells.extent(0);

  // We also need to consider faces that connect to cells that do not exist, but are needed for boundary conditions
  // We dub them virtual cell since there should be no geometry associated with them, or topology really
  // They exist only for datastorage so that they are consistent with 'real' cells from an algorithm perspective

  // Each virtual face (face linked to a '-1' cell) requires a virtual cell (i.e. turn the '-1' into a virtual cell)
  // Virtual cells are those that do not exist but are connected to an owned cell
  // Note - in the future, ghosted cells will also need to connect to virtual cells at boundary conditions, but for the moment we will ignore this.

  // Iterate over all faces and identify the faces connected to a potential virtual cell
  std::vector<int> all_boundary_faces;
  const int num_faces = elems_by_face.extent(0);
  auto elems_by_face_h = Kokkos::create_mirror_view(elems_by_face);
  Kokkos::deep_copy(elems_by_face_h, elems_by_face);
  for(int face=0;face<num_faces;++face)
    if(elems_by_face_h(face,0) < 0 or elems_by_face_h(face,1) < 0)
      all_boundary_faces.push_back(face);
  const panzer::LocalOrdinal num_virtual_cells = all_boundary_faces.size();

  // Create some global indexes associated with the virtual cells
  // Note: We are assuming that virtual cells belong to ranks and are not 'shared' - this will change later on
  virtual_cells = PHX::View<panzer::GlobalOrdinal*>("virtual_cells",num_virtual_cells);
  auto virtual_cells_h = Kokkos::create_mirror_view(virtual_cells);
  {
    PANZER_FUNC_TIME_MONITOR_DIFF("Initial global index creation",InitialGlobalIndexCreation);

    const int num_ranks = comm->getSize();
    const int rank = comm->getRank();

    std::vector<panzer::GlobalOrdinal> owned_cell_distribution(num_ranks,0);
    {
      std::vector<panzer::GlobalOrdinal> my_owned_cell_distribution(num_ranks,0);
      my_owned_cell_distribution[rank] = num_owned_cells;

      Teuchos::reduceAll(*comm,Teuchos::REDUCE_SUM, num_ranks, my_owned_cell_distribution.data(),owned_cell_distribution.data());
    }

    std::vector<panzer::GlobalOrdinal> virtual_cell_distribution(num_ranks,0);
    {
      std::vector<panzer::GlobalOrdinal> my_virtual_cell_distribution(num_ranks,0);
      my_virtual_cell_distribution[rank] = num_virtual_cells;

      Teuchos::reduceAll(*comm,Teuchos::REDUCE_SUM, num_ranks, my_virtual_cell_distribution.data(),virtual_cell_distribution.data());
    }

    panzer::GlobalOrdinal num_global_real_cells=0;
    for(int i=0;i<num_ranks;++i){
      num_global_real_cells+=owned_cell_distribution[i];
    }

    panzer::GlobalOrdinal global_virtual_start_idx = num_global_real_cells;
    for(int i=0;i<rank;++i){
      global_virtual_start_idx += virtual_cell_distribution[i];
    }

    for(int i=0;i<num_virtual_cells;++i){
      virtual_cells_h(i) = global_virtual_start_idx + panzer::GlobalOrdinal(i);
    }
  }
  Kokkos::deep_copy(virtual_cells, virtual_cells_h);
}

}
