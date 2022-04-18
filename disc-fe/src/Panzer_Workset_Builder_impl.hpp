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

#ifndef __Panzer_Workset_Builder_impl_hpp__
#define __Panzer_Workset_Builder_impl_hpp__

#include <iostream>
#include <vector>
#include <map>
#include <algorithm>
#include "Panzer_Workset.hpp"
#include "Panzer_CellData.hpp"
#include "Panzer_BC.hpp"
#include "Panzer_CommonArrayFactories.hpp"

#include "Phalanx_DataLayout_MDALayout.hpp"

// Intrepid2
#include "Intrepid2_DefaultCubatureFactory.hpp"
#include "Intrepid2_CellTools.hpp"
#include "Intrepid2_FunctionSpaceTools.hpp"
#include "Intrepid2_Basis.hpp"

template<typename ArrayT>
Teuchos::RCP< std::vector<panzer::Workset> >
panzer::buildWorksets(const WorksetNeeds & needs,
                      const std::string & elementBlock,
		              const std::vector<std::size_t>& local_cell_ids,
		              const ArrayT& vertex_coordinates)
{
  panzer::MDFieldArrayFactory mdArrayFactory("",true);

  std::size_t total_num_cells = local_cell_ids.size();

  std::size_t workset_size = needs.cellData.numCells();

  Teuchos::RCP< std::vector<panzer::Workset> > worksets_ptr =
    Teuchos::rcp(new std::vector<panzer::Workset>);
  std::vector<panzer::Workset>& worksets = *worksets_ptr;

  // special case for 0 elements!
  if(total_num_cells==0) {
     worksets.resize(1);
     std::vector<panzer::Workset>::iterator i = worksets.begin();
     i->setNumberOfCells(0,0,0);
     i->block_id = elementBlock;
     i->ir_degrees.clear();
	 i->basis_names.clear();

     for (std::size_t j=0;j<needs.int_rules.size();j++) {

       Teuchos::RCP<panzer::IntegrationValues2<double> > iv2 =
	 Teuchos::rcp(new panzer::IntegrationValues2<double>("",true));
       iv2->setupArrays(needs.int_rules[j]);

       i->ir_degrees.emplace_back(needs.int_rules[j]->cubature_degree);
       i->int_rules.push_back(iv2);
     }

     // Need to create all combinations of basis/ir pairings
     for (std::size_t j=0;j<needs.int_rules.size();j++) {
       for (std::size_t b=0;b<needs.bases.size();b++) {
	 Teuchos::RCP<panzer::BasisIRLayout> b_layout
             = Teuchos::rcp(new panzer::BasisIRLayout(needs.bases[b],*needs.int_rules[j]));

	 Teuchos::RCP<panzer::BasisValues2<double> > bv2
             = Teuchos::rcp(new panzer::BasisValues2<double>("",true,true));
	 bv2->setupArrays(b_layout);
	 i->bases.push_back(bv2);

	 i->basis_names.emplace_back(b_layout->name());
       }

     }

     return worksets_ptr;
  } // end special case

  {
    std::size_t num_worksets = total_num_cells / workset_size;
    bool last_set_is_full = true;
    std::size_t last_workset_size = total_num_cells % workset_size;
    if (last_workset_size != 0) {
      num_worksets += 1;
      last_set_is_full = false;
    }

    worksets.resize(num_worksets);
    std::vector<panzer::Workset>::iterator i;
    for (i = worksets.begin(); i != worksets.end(); ++i)
      i->setNumberOfCells(workset_size,0,0);

    if (!last_set_is_full) {
      worksets.back().setNumberOfCells(last_workset_size,0,0);
    }
  }

  // assign workset cell local ids
  std::vector<std::size_t>::const_iterator local_begin = local_cell_ids.begin();
  for (std::vector<panzer::Workset>::iterator wkst = worksets.begin(); wkst != worksets.end(); ++wkst) {
    std::vector<std::size_t>::const_iterator begin_iter = local_begin;
    std::vector<std::size_t>::const_iterator end_iter = begin_iter + wkst->num_cells;
    local_begin = end_iter;
    wkst->cell_local_ids.assign(begin_iter,end_iter);

    PHX::View<int*> cell_local_ids_k = PHX::View<int*>("Workset:cell_local_ids",wkst->cell_local_ids.size());
    auto cell_local_ids_k_h = Kokkos::create_mirror_view(cell_local_ids_k);
    for(std::size_t i=0;i<wkst->cell_local_ids.size();i++)
      cell_local_ids_k_h(i) = wkst->cell_local_ids[i];
    Kokkos::deep_copy(cell_local_ids_k, cell_local_ids_k_h);
    wkst->cell_local_ids_k = cell_local_ids_k;

    wkst->cell_vertex_coordinates = mdArrayFactory.buildStaticArray<double,Cell,NODE,Dim>("cvc",workset_size,
					 vertex_coordinates.extent(1),
					 vertex_coordinates.extent(2));
    wkst->block_id = elementBlock;
    wkst->subcell_dim = needs.cellData.baseCellDimension();
    wkst->subcell_index = 0;
  }

  TEUCHOS_ASSERT(local_begin == local_cell_ids.end());

  // Copy cell vertex coordinates into local workset arrays
  std::size_t offset = 0;
  for (std::vector<panzer::Workset>::iterator wkst = worksets.begin(); wkst != worksets.end(); ++wkst) {
    auto cell_vertex_coordinates = wkst->cell_vertex_coordinates.get_static_view();
    Kokkos::parallel_for(wkst->num_cells, KOKKOS_LAMBDA (int cell) {
      for (std::size_t vertex = 0; vertex < vertex_coordinates.extent(1); ++ vertex)
	      for (std::size_t dim = 0; dim < vertex_coordinates.extent(2); ++ dim) {
	      //wkst->cell_vertex_coordinates(cell,vertex,dim) = vertex_coordinates(cell + offset,vertex,dim);
	        cell_vertex_coordinates(cell,vertex,dim) = vertex_coordinates(cell + offset,vertex,dim);
        }
      });
    Kokkos::fence();
    offset += wkst->num_cells;
  }

  TEUCHOS_ASSERT(offset == Teuchos::as<std::size_t>(vertex_coordinates.extent(0)));

  // setup the integration rules and bases
  for(std::vector<panzer::Workset>::iterator wkst = worksets.begin(); wkst != worksets.end(); ++wkst)
    populateValueArrays(wkst->num_cells,false,needs,*wkst);

  return worksets_ptr;
}

// ****************************************************************
// ****************************************************************

template<typename ArrayT>
Teuchos::RCP<std::map<unsigned,panzer::Workset> >
panzer::buildBCWorkset(const WorksetNeeds & needs,
                       const std::string & elementBlock,
                       const std::vector<std::size_t>& local_cell_ids,
                       const std::vector<std::size_t>& local_side_ids,
                       const ArrayT& vertex_coordinates,
                       const bool populate_value_arrays)
{
  using Teuchos::RCP;
  using Teuchos::rcp;

  panzer::MDFieldArrayFactory mdArrayFactory("",true);

  // key is local face index, value is workset with all elements
  // for that local face
  auto worksets_ptr = Teuchos::rcp(new std::map<unsigned,panzer::Workset>);

  // All elements of boundary condition should go into one workset.
  // However due to design of Intrepid2 (requires same basis for all
  // cells), we have to separate the workset based on the local side
  // index.  Each workset for a boundary condition is associated with
  // a local side for the element

  TEUCHOS_ASSERT(local_side_ids.size() == local_cell_ids.size());
  TEUCHOS_ASSERT(local_side_ids.size() == static_cast<std::size_t>(vertex_coordinates.extent(0)));

  // key is local face index, value is a pair of cell index and vector of element local ids
  std::map< unsigned, std::vector< std::pair< std::size_t, std::size_t>>> element_list;
  for (std::size_t cell = 0; cell < local_cell_ids.size(); ++cell)
    element_list[local_side_ids[cell]].push_back(std::make_pair(cell, local_cell_ids[cell]));

  auto& worksets = *worksets_ptr;

  // create worksets
  for (const auto& side : element_list) {

    auto& cell_local_ids = worksets[side.first].cell_local_ids;

    worksets[side.first].cell_vertex_coordinates = mdArrayFactory.buildStaticArray<double,Cell,NODE,Dim>("cvc",
                                                          side.second.size(),
                                                          vertex_coordinates.extent(1),
                                                          vertex_coordinates.extent(2));
    auto coords_view = worksets[side.first].cell_vertex_coordinates.get_view();
    auto coords_h = Kokkos::create_mirror_view(coords_view);

    auto vertex_coordinates_h = Kokkos::create_mirror_view(PHX::as_view(vertex_coordinates));
    Kokkos::deep_copy(vertex_coordinates_h, PHX::as_view(vertex_coordinates));

    for (std::size_t cell = 0; cell < side.second.size(); ++cell) {
      cell_local_ids.push_back(side.second[cell].second);
      const auto dim0 = side.second[cell].first;

      for(std::size_t vertex = 0; vertex < vertex_coordinates.extent(1); ++vertex)
      {
        const auto extent = Teuchos::as<std::size_t>(vertex_coordinates.extent(2));

        for (std::size_t dim = 0; dim < extent; ++dim)
          coords_h(cell, vertex, dim) = vertex_coordinates_h(dim0, vertex, dim);
      }
    }

    Kokkos::deep_copy(coords_view, coords_h);

    const auto cell_local_ids_size = worksets[side.first].cell_local_ids.size();
    auto cell_local_ids_k = PHX::View<int*>("Workset:cell_local_ids", cell_local_ids_size);
    auto cell_local_ids_k_h = Kokkos::create_mirror_view(cell_local_ids_k);

    for(std::size_t i = 0; i < cell_local_ids_size; ++i){
      cell_local_ids_k_h(i) = worksets.at(side.first).cell_local_ids[i];
    }

    Kokkos::deep_copy(cell_local_ids_k, cell_local_ids_k_h);

    worksets[side.first].cell_local_ids_k = cell_local_ids_k;
    worksets[side.first].num_cells = worksets[side.first].cell_local_ids.size();
    worksets[side.first].block_id = elementBlock;
    worksets[side.first].subcell_dim = needs.cellData.baseCellDimension() - 1;
    worksets[side.first].subcell_index = side.first;
  }

  if (populate_value_arrays) {
    // setup the integration rules and bases
    for (std::map<unsigned,panzer::Workset>::iterator wkst = worksets.begin();
         wkst != worksets.end(); ++wkst) {

      populateValueArrays(wkst->second.num_cells,true,needs,wkst->second); // populate "side" values
    }
  }

  return worksets_ptr;
}

// ****************************************************************
// ****************************************************************

namespace panzer {
namespace impl {

/* Associate two sets of local side IDs into lists. Each list L has the property
 * that every local side id in that list is the same, and this holds for each
 * local side ID set. The smallest set of lists is found. The motivation for
 * this procedure is to find a 1-1 workset pairing in advance. See the comment
 * re: Intrepid2 in buildBCWorkset for more.
 *   The return value is an RCP to a map. Only the map's values are of interest
 * in practice. Each value is a list L. The map's key is a pair (side ID a, side
 * ID b) that gives rise to the list. We return a pointer to a map so that the
 * caller does not have to deal with the map type; auto suffices.
 */
Teuchos::RCP< std::map<std::pair<std::size_t, std::size_t>, std::vector<std::size_t> > >
associateCellsBySideIds(const std::vector<std::size_t>& sia /* local_side_ids_a */,
                        const std::vector<std::size_t>& sib /* local_side_ids_b */)
{
  TEUCHOS_ASSERT(sia.size() == sib.size());

  auto sip2i_p = Teuchos::rcp(new std::map< std::pair<std::size_t, std::size_t>, std::vector<std::size_t> >);
  auto& sip2i = *sip2i_p;

  for (std::size_t i = 0; i < sia.size(); ++i)
    sip2i[std::make_pair(sia[i], sib[i])].push_back(i);

  return sip2i_p;
}

// Set s = a(idxs). No error checking.
template <typename T>
void subset(const std::vector<T>& a, const std::vector<std::size_t>& idxs, std::vector<T>& s)
{
  s.resize(idxs.size());
  for (std::size_t i = 0; i < idxs.size(); ++i)
    s[i] = a[idxs[i]];
}

template<typename ArrayT>
Teuchos::RCP<std::map<unsigned,panzer::Workset> >
buildBCWorksetForUniqueSideId(const panzer::WorksetNeeds & needs_a,
                              const std::string & blockid_a,
                              const std::vector<std::size_t>& local_cell_ids_a,
                              const std::vector<std::size_t>& local_side_ids_a,
                              const ArrayT& vertex_coordinates_a,
                              const panzer::WorksetNeeds & needs_b,
                              const std::string & blockid_b,
                              const std::vector<std::size_t>& local_cell_ids_b,
                              const std::vector<std::size_t>& local_side_ids_b,
                              const ArrayT& vertex_coordinates_b,
                              const WorksetNeeds& needs_b2)
{
  TEUCHOS_ASSERT(local_cell_ids_a.size() == local_cell_ids_b.size());
  // Get a and b workset maps separately, but don't populate b's arrays.
  const Teuchos::RCP<std::map<unsigned,panzer::Workset> >
    mwa = buildBCWorkset(needs_a,blockid_a, local_cell_ids_a, local_side_ids_a, vertex_coordinates_a),
    mwb = buildBCWorkset(needs_b2, blockid_b, local_cell_ids_b, local_side_ids_b,
                         vertex_coordinates_b, false /* populate_value_arrays */);
  TEUCHOS_ASSERT(mwa->size() == 1 && mwb->size() == 1);
  for (std::map<unsigned,panzer::Workset>::iterator ait = mwa->begin(), bit = mwb->begin();
       ait != mwa->end(); ++ait, ++bit) {
    TEUCHOS_ASSERT(Teuchos::as<std::size_t>(ait->second.num_cells) == local_cell_ids_a.size() &&
                   Teuchos::as<std::size_t>(bit->second.num_cells) == local_cell_ids_b.size());
    panzer::Workset& wa = ait->second;
    // Copy b's details(0) to a's details(1).
    wa.other = Teuchos::rcp(new panzer::WorksetDetails(bit->second.details(0)));
    // Populate details(1) arrays so that IP are in order corresponding to details(0).
    populateValueArrays(wa.num_cells, true, needs_b, wa.details(1), Teuchos::rcpFromRef(wa.details(0)));
  }
  // Now mwa has everything we need.
  return mwa;
}

} // namespace impl
} // namespace panzer

// ****************************************************************
// ****************************************************************

template<typename ArrayT>
Teuchos::RCP<std::map<unsigned,panzer::Workset> >
panzer::buildBCWorkset(const WorksetNeeds & needs_a,
                       const std::string & blockid_a,
                       const std::vector<std::size_t>& local_cell_ids_a,
                       const std::vector<std::size_t>& local_side_ids_a,
                       const ArrayT& vertex_coordinates_a,
                       const panzer::WorksetNeeds & needs_b,
                       const std::string & blockid_b,
                       const std::vector<std::size_t>& local_cell_ids_b,
                       const std::vector<std::size_t>& local_side_ids_b,
                       const ArrayT& vertex_coordinates_b)
{
  // Since Intrepid2 requires all side IDs in a workset to be the same (see
  // Intrepid2 comment above), we break the element list into pieces such that
  // each piece contains elements on each side of the interface L_a and L_b and
  // all elemnets L_a have the same side ID, and the same for L_b.
  auto side_id_associations = impl::associateCellsBySideIds(local_side_ids_a, local_side_ids_b);
  if (side_id_associations->size() == 1) {
    // Common case of one workset on each side; optimize for it.
    return impl::buildBCWorksetForUniqueSideId(needs_a, blockid_a, local_cell_ids_a, local_side_ids_a, vertex_coordinates_a,
                                               needs_b, blockid_b, local_cell_ids_b, local_side_ids_b, vertex_coordinates_b,
                                               needs_b);
  } else {
    // The interface has elements having a mix of side IDs, so deal with each
    // pair in turn.
    Teuchos::RCP<std::map<unsigned,panzer::Workset> > mwa = Teuchos::rcp(new std::map<unsigned,panzer::Workset>);
    std::vector<std::size_t> lci_a, lci_b, lsi_a, lsi_b;
    panzer::MDFieldArrayFactory mdArrayFactory("", true);
    const int d1 = Teuchos::as<std::size_t>(vertex_coordinates_a.extent(1)),
      d2 = Teuchos::as<std::size_t>(vertex_coordinates_a.extent(2));
    for (auto it : *side_id_associations) {
      const auto& idxs = it.second;
      impl::subset(local_cell_ids_a, idxs, lci_a);
      impl::subset(local_side_ids_a, idxs, lsi_a);
      impl::subset(local_cell_ids_b, idxs, lci_b);
      impl::subset(local_side_ids_b, idxs, lsi_b);
      auto vc_a = mdArrayFactory.buildStaticArray<double,Cell,NODE,Dim>("vc_a", idxs.size(), d1, d2);
      auto vc_b = mdArrayFactory.buildStaticArray<double,Cell,NODE,Dim>("vc_b", idxs.size(), d1, d2);
      auto vc_a_h = Kokkos::create_mirror_view(vc_a.get_static_view());
      auto vc_b_h = Kokkos::create_mirror_view(vc_b.get_static_view());
      auto vertex_coordinates_a_h = Kokkos::create_mirror_view(PHX::as_view(vertex_coordinates_a));
      auto vertex_coordinates_b_h = Kokkos::create_mirror_view(PHX::as_view(vertex_coordinates_b));
      Kokkos::deep_copy(vertex_coordinates_a_h, PHX::as_view(vertex_coordinates_a));
      Kokkos::deep_copy(vertex_coordinates_b_h, PHX::as_view(vertex_coordinates_b));
      for (std::size_t i = 0; i < idxs.size(); ++i) {
        const auto ii = idxs[i];
        for (int j = 0; j < d1; ++j)
          for (int k = 0; k < d2; ++k) {
            vc_a_h(i, j, k) = vertex_coordinates_a_h(ii, j, k);
            vc_b_h(i, j, k) = vertex_coordinates_b_h(ii, j, k);
          }
      }
      Kokkos::deep_copy(vc_a.get_static_view(), vc_a_h);
      Kokkos::deep_copy(vc_b.get_static_view(), vc_b_h);
      auto mwa_it = impl::buildBCWorksetForUniqueSideId(needs_a,blockid_a, lci_a, lsi_a, vc_a,
                                                        needs_b,blockid_b, lci_b, lsi_b, vc_b,
                                                        needs_b);
      TEUCHOS_ASSERT(mwa_it->size() == 1);
      // Form a unique key that encodes the pair (side ID a, side ID b). We
      // abuse the key here in the sense that it is everywhere else understood
      // to correspond to the side ID of the elements in the workset.
      //   1000 is a number substantially larger than is needed for any element.
      const std::size_t key = lsi_a[0] * 1000 + lsi_b[0];
      (*mwa)[key] = mwa_it->begin()->second;
    }
    return mwa;
  }
}

#endif
