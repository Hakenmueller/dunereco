////////////////////////////////////////////////////////////////////////
/// \file    CTPHelper.cxx
/// \brief   Functions to help use the convolutional track PID
/// \author  Leigh Whitehead - leigh.howard.whitehead@cern.ch
////////////////////////////////////////////////////////////////////////

#include <vector>
#include <string>
#include <random>

#include "TVector3.h"

#include "art/Framework/Principal/Event.h"
#include "canvas/Persistency/Common/FindManyP.h"
#include "nusimdata/SimulationBase/MCParticle.h"
#include "lardataobj/RecoBase/Track.h"
#include "lardataobj/RecoBase/PFParticle.h"
#include "lardataobj/AnalysisBase/Calorimetry.h"

#include "dune/TrackPID/CTPHelper.h"
#include "dune/TrackPID/CTPResult.h"
#include "dune/TrackPID/tf/CTPGraph.h"

#include "dune/AnaUtils/DUNEAnaPFParticleUtils.h"
#include "dune/AnaUtils/DUNEAnaTrackUtils.h"

#include "cetlib/getenv.h"

namespace ctp
{

  // Constructor
  CTPHelper::CTPHelper(const fhicl::ParameterSet& pset){
    fNetDir  = cet::getenv(pset.get<std::string>("NetworkPath"));
    fNetName = fNetDir + pset.get<std::string>("NetworkName");
    fParticleLabel = pset.get<std::string>("ParticleLabel");
    fTrackLabel = pset.get<std::string>("TrackLabel");
    fCalorimetryLabel = pset.get<std::string>("CalorimetryLabel");
    fMinTrackPoints = pset.get<unsigned int>("MinHits",50);
    fDedxLength = pset.get<unsigned int>("DedxLength",100);
    fQMax = pset.get<float>("MaxCharge",1000);
    fQJump = pset.get<float>("MaxChargeJump",500);
  }

  // Function to calculate the PID for a given track
  const ctp::CTPResult CTPHelper::RunConvolutionalTrackPID(const art::Ptr<recob::PFParticle> part, const art::Event &evt) const{

    // Get the inputs to the network
    std::vector< std::vector< std::vector<float> > > finalInputs;

    std::vector< std::vector<float> > twoVecs = GetNetworkInputs(part,evt);
    finalInputs.push_back(twoVecs);

    // Load the network and run it    
    std::unique_ptr<tf::CTPGraph> convNet = tf::CTPGraph::create(fNetName.c_str(),std::vector<std::string>(),2,1);
    std::vector< std::vector< std::vector<float> > > convNetOutput = convNet->run(finalInputs);

    ctp::CTPResult result(convNetOutput.at(0).at(0));

    return result;
  }

  // Calculate the features for the track PID
  const std::vector<std::vector<float>> CTPHelper::GetNetworkInputs(const art::Ptr<recob::PFParticle> part, const art::Event &evt) const{
  
    std::vector<std::vector<float>> netInputs;

    if(!dune_ana::DUNEAnaPFParticleUtils::IsTrack(part,evt,fParticleLabel,fTrackLabel)){
      std::cout << "CTPHelper: this PFParticle is not track-like... returning empty vector." << std::endl;
      return netInputs;
    }

    // Use the analysis utilities to simplify finding products and associations
    art::Ptr<recob::Track> thisTrack = dune_ana::DUNEAnaPFParticleUtils::GetTrack(part,evt,fParticleLabel,fTrackLabel);
    art::Ptr<anab::Calorimetry> thisCalo = dune_ana::DUNEAnaTrackUtils::GetCalorimetry(thisTrack,evt,fTrackLabel,fCalorimetryLabel);

    if(thisCalo->dEdx().size() < fMinTrackPoints){
      std::cout << "CTPHelper: this track has too few points for PID... returning empty vector." << std::endl;
      return netInputs;
    }

    std::vector<float> dedxVector = thisCalo->dEdx();
    this->SmoothDedxVector(dedxVector);
    float dedxMean = 0.;
    float dedxSigma = 0.;

    // We want to use the middle third of the dedx vector (with max length 100)
    std::vector<float> dedxTrunc;
    unsigned int pointsForAverage = (fDedxLength - fMinTrackPoints) / 3;
    unsigned int avStart = dedxVector.size() - 1 - pointsForAverage;
    unsigned int avEnd   = dedxVector.size() - 1 - (2*pointsForAverage);
    for(unsigned int e = avStart; e > avEnd; --e) dedxTrunc.push_back(dedxVector.at(e));
    this->GetDedxMeanAndSigma(dedxTrunc,dedxMean,dedxSigma);

    // If our dedx vector is between fMinTrackPoints and fDedxLength in size then we need to pad it
    if(dedxVector.size() < fMinTrackPoints){
      this->PadDedxVector(dedxVector,dedxMean,dedxSigma);
    }
    
    std::vector<float> finalInputDedx;
    finalInputDedx.insert(finalInputDedx.begin(),dedxVector.end() - fDedxLength,dedxVector.end());

    std::vector<float> finalInputVariables;
    // Get the number of child particles
    unsigned int nTrack, nShower, nGrand;
    this->GetChildParticles(part,evt,nTrack,nShower,nGrand);
    finalInputVariables.push_back(nTrack);
    finalInputVariables.push_back(nShower);
    finalInputVariables.push_back(nGrand);
    // Now add the dedx mean and sigma
    finalInputVariables.push_back(dedxMean);
    finalInputVariables.push_back(dedxSigma);
    // Finally, get the angular deflection mean and sigma
    float deflectionMean, deflectionSigma;
    this->GetDeflectionMeanAndSigma(thisTrack,deflectionMean,deflectionSigma);
    finalInputVariables.push_back(deflectionMean);
    finalInputVariables.push_back(deflectionSigma);
  
    netInputs.push_back(finalInputDedx);
    netInputs.push_back(finalInputVariables);

    return netInputs;
  }

  const std::vector<float> CTPHelper::GetDeDxVector(const art::Ptr<recob::PFParticle> part, const art::Event &evt) const{
    return GetNetworkInputs(part,evt).at(0);
  }

  const std::vector<float> CTPHelper::GetVariableVector(const art::Ptr<recob::PFParticle> part, const art::Event &evt) const{
    return GetNetworkInputs(part,evt).at(1);
  }
 
  void CTPHelper::SmoothDedxVector(std::vector<float> &dedx) const{

    // Firstly, get rid of all very high values > fQMax
    for(float val : dedx){
      if(val > fQMax) val = fQMax;
      if(val < 0.) val = 0.;
    }

    // Now try to smooth over jumps
    unsigned int nQ = dedx.size();
    // First and last points are special cases
    if((dedx[0] - dedx[1]) > fQJump) dedx[0] = dedx[1] + (dedx[1] - dedx[2]);
    if((dedx[nQ-1] - dedx[nQ-2]) > fQJump) dedx[nQ-1] = dedx[nQ-2] + (dedx[nQ-2] - dedx[nQ-3]);
    // Now do the rest of the points
    for(unsigned int q = 1; q < nQ - 1; ++q){
      if((dedx[q] - dedx[q-1]) > fQJump)
      {
        dedx[q] = 0.5 * (dedx[q-1]+dedx[q+1]);
      }
    }

  }

  void CTPHelper::PadDedxVector(std::vector<float> &dedx, const float mean, const float sigma) const{

    std::default_random_engine generator;
    std::normal_distribution<float> gaussDist(mean,sigma);

    unsigned int originalSize = dedx.size();
    for (unsigned int h = 0; h + originalSize < fDedxLength; ++h)
    {
      // Pick a random Gaussian value but ensure we don't go negative
      float randVal = -1;
      do
      {
        randVal = gaussDist(generator);
      }
      while (randVal < 0);
      // Pad from beginning to keep the real track part at the end
      dedx.insert(dedx.begin(),randVal);
    }

  } 

  void CTPHelper::GetDedxMeanAndSigma(const std::vector<float> &dedx, float &mean, float &sigma) const{

    float averageDedx = 0;
    float sigmaDedx = 0;
    for(const float &q : dedx) averageDedx += q;
    mean = averageDedx / static_cast<float>(dedx.size());
    for(const float &q : dedx) sigmaDedx += (mean -q)*(mean-q);
    sigma = std::sqrt(sigmaDedx / static_cast<float>(dedx.size()));
  }

  void CTPHelper::GetDeflectionMeanAndSigma(const art::Ptr<recob::Track> track, float &mean, float &sigma) const{

    std::vector<float> trajAngle;
    for(unsigned int p = 1; p < track->Trajectory().NPoints(); ++p){
      TVector3 thisDir = track->Trajectory().DirectionAtPoint<TVector3>(p);
      TVector3 prevDir = track->Trajectory().DirectionAtPoint<TVector3>(p-1);
      trajAngle.push_back(thisDir.Angle(prevDir));
    }

    // Average and sigma of the angular deflection between trajectory points (wobble)
    float averageAngle = 0;
    float standardDevAngle = 0;
    for(const float &a : trajAngle) averageAngle += a;
    mean = averageAngle / static_cast<float>(trajAngle.size());
    for(const float &a : trajAngle) standardDevAngle += (mean - a)*(mean - a);
    sigma = sqrt(standardDevAngle / static_cast<float>(trajAngle.size()));
  }

  void CTPHelper::GetChildParticles(const art::Ptr<recob::PFParticle> part, const art::Event &evt, unsigned int &nTrack, unsigned int &nShower, unsigned int &nGrand) const{

    nTrack = 0;
    nShower = 0;
    nGrand = 0;

    std::vector<art::Ptr<recob::PFParticle>> children = dune_ana::DUNEAnaPFParticleUtils::GetChildParticles(part,evt,fParticleLabel);

    for(const art::Ptr<recob::PFParticle> child : children){
      nTrack += dune_ana::DUNEAnaPFParticleUtils::IsTrack(part,evt,fParticleLabel,fTrackLabel);
      nShower += dune_ana::DUNEAnaPFParticleUtils::IsShower(part,evt,fParticleLabel,fTrackLabel);
      nGrand += child->NumDaughters();
    }

  }

}

