// @HEADER
// ***********************************************************************
//
//           TianXin: A partial differential equation assembly
//       engine for strongly coupled complex multiphysics systems
//                 Copyright (2022) YUAN Xi
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

#ifndef _TIANXIN_NEUMANN_EVALUATOR_HPP
#define _TIANXIN_NEUMANN_EVALUATOR_HPP

#include "Phalanx_Evaluator_Macros.hpp"
#include "Phalanx_MDField.hpp"

#include "TianXin_WorksetFunctor.hpp"

#include <memory>

namespace TianXin {
    
  /** \brief Evaluates a Neumann BC residual contribution

      computes the surface integral term resulting from integration
      by parts for a particular dof:

      int(n \cdot (flux * phi) )
  */
template<typename EvalT, typename Traits>
class NeumannBase : public PHX::EvaluatorWithBaseImpl<Traits>
{
  public:

    NeumannBase(const Teuchos::ParameterList& p);

    void postRegistrationSetup(typename Traits::SetupData d,PHX::FieldManager<Traits>& fm);
    virtual void evaluateFields(typename Traits::EvalData d) = 0;
	
  protected:
	std::unique_ptr<TianXin::WorksetFunctor> pFunc;

  private:

    using ScalarT = typename EvalT::ScalarT;
  
  PHX::MDField<ScalarT> residual;
  PHX::MDField<ScalarT> normal_dot_flux;
  PHX::MDField<const ScalarT> flux;
  PHX::MDField<const ScalarT> normal;
  
  // output
  Kokkos::DynRankView<ScalarT, PHX::Device> neumann;

  std::string basis_name;
  std::size_t basis_index;
  std::size_t num_ip;
  std::size_t num_dim;

}; // end of class NeumannBase


}

#endif
