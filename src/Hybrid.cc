#include "HiggsAnalysis/CombinedLimit/interface/Hybrid.h"
#include "RooRealVar.h"
#include "RooArgSet.h"
#include "RooStats/HybridCalculatorOriginal.h"
#include "RooAbsPdf.h"
#include "HiggsAnalysis/CombinedLimit/interface/Combine.h"

using namespace RooStats;

Hybrid::Hybrid() : 
LimitAlgo("Hybrid specific options") {
    options_.add_options()
        ("toysH,T", boost::program_options::value<unsigned int>(&nToys_)->default_value(500),    "Number of Toy MC extractions to compute CLs+b, CLb and CLs")
        ("clsAcc",  boost::program_options::value<double>(&clsAccuracy_ )->default_value(0.005), "Absolute accuracy on CLs to reach to terminate the scan")
        ("rAbsAcc", boost::program_options::value<double>(&rAbsAccuracy_)->default_value(0.1),   "Absolute accuracy on r to reach to terminate the scan")
        ("rRelAcc", boost::program_options::value<double>(&rRelAccuracy_)->default_value(0.05),  "Relative accuracy on r to reach to terminate the scan")
        ("cls",     boost::program_options::value<bool>(&CLs_)->default_value(true),  "Use CLs if true (default), CLsplusb if false")
        ("testStat",boost::program_options::value<std::string>(&testStat_)->default_value("LEP"),  "Test statistics: LEP, TEV.")
        ("rInterval", "Always try to compute an interval on r even after having found a point satisfiying the CL")
    ;
}

void Hybrid::applyOptions(const boost::program_options::variables_map &vm) {
    rInterval_ = vm.count("rInterval");
    if (testStat_ != "LEP" && testStat_ != "TEV") {
        std::cerr << "Error: test statistics should be one of 'LEP' or 'TEV', and not '" << testStat_ << "'" << std::endl;
        abort();
    }
}

bool Hybrid::run(RooWorkspace *w, RooAbsData &data, double &limit, const double *hint) {
  RooRealVar *r = w->var("r"); r->setConstant(true);
  RooArgSet  poi(*r);
  w->loadSnapshot("clean");
  RooAbsPdf *altModel  = w->pdf("model_s"), *nullModel = w->pdf("model_b");
  
  HybridCalculatorOriginal* hc = new HybridCalculatorOriginal(data,*altModel,*nullModel);
  if (withSystematics) {
    if ((w->set("nuisances") == 0) || (w->pdf("nuisancePdf") == 0)) {
          std::cerr << "ERROR: nuisances or nuisancePdf not set. Perhaps you wanted to run with no systematics?\n" << std::endl;
          abort();
    }
    hc->UseNuisance(true);
    hc->SetNuisancePdf(*w->pdf("nuisancePdf"));
    hc->SetNuisanceParameters(*w->set("nuisances"));
  } else {
    hc->UseNuisance(false);
  }
  hc->SetTestStatistic(testStat_ == "LEP" ? 1 : 3); // 3 = TeV
  hc->PatchSetExtended(w->pdf("model_b")->canBeExtended()); // Number counting, each dataset has 1 entry 
  hc->SetNumberOfToys(nToys_);
  
  if ((hint != 0) && (*hint > r->getMin())) {
    r->setMax(std::min<double>(3*(*hint), r->getMax()));
  }
  
  typedef std::pair<double,double> CLs_t;

  double clsTarget = 1 - cl; 
  CLs_t clsMin(1,0), clsMax(0,0);
  double rMin = 0, rMax = r->getMax();

  std::cout << "Search for upper limit to the limit" << std::endl;
  for (;;) {
    CLs_t clsMax = eval(r, r->getMax(), hc);
    if (clsMax.first == 0 || clsMax.first + 3 * fabs(clsMax.second) < cl ) break;
    r->setMax(r->getMax()*2);
    if (r->getVal()/rMax >= 20) { 
      std::cerr << "Cannot set higher limit: at r = " << r->getVal() << " still get " << (CLs_ ? "CLs" : "CLsplusb") << " = " << clsMax.first << std::endl;
      return false;
    }
  }
  rMax = r->getMax();
  
  std::cout << "Now doing proper bracketing & bisection" << std::endl;
  bool lucky = false;
  do {
    CLs_t clsMid = eval(r, 0.5*(rMin+rMax), hc, true, clsTarget);
    if (clsMid.second == -1) {
      std::cerr << "Hypotest failed" << std::endl;
      return false;
    }
    if (fabs(clsMid.first-clsTarget) <= clsAccuracy_) {
      std::cout << "reached accuracy." << std::endl;
      lucky = true;
      break;
    }
    if ((clsMid.first>clsTarget) == (clsMax.first>clsTarget)) {
      rMax = r->getVal(); clsMax = clsMid;
    } else {
      rMin = r->getVal(); clsMin = clsMid;
    }
  } while (rMax-rMin > std::max(rAbsAccuracy_, rRelAccuracy_ * r->getVal()));

  if (lucky) {
    limit = r->getVal();
    if (rInterval_) {
      std::cout << "\n -- HypoTestInverter (before determining interval) -- \n";
      std::cout << "Limit: r < " << limit << " +/- " << 0.5*(rMax - rMin) << " @ " <<cl * 100<<"% CL\n";

      double rBoundLow  = limit - 0.5*std::max(rAbsAccuracy_, rRelAccuracy_ * limit);
      for (r->setVal(rMin); r->getVal() < rBoundLow  && (fabs(clsMin.first-clsTarget) >= clsAccuracy_); rMin = r->getVal()) {
        clsMax = eval(r, 0.5*(r->getVal()+limit), hc, true, clsTarget);
      }

      double rBoundHigh = limit + 0.5*std::max(rAbsAccuracy_, rRelAccuracy_ * limit);
      for (r->setVal(rMax); r->getVal() > rBoundHigh && (fabs(clsMax.first-clsTarget) >= clsAccuracy_); rMax = r->getVal()) {
        clsMax = eval(r, 0.5*(r->getVal()+limit), hc, true, clsTarget);
      }
    }
  } else {
    limit = 0.5*(rMax+rMin);
  }
  std::cout << "\n -- HypoTestInverter -- \n";
  std::cout << "Limit: r < " << limit << " +/- " << 0.5*(rMax - rMin) << " @ " <<cl * 100<<"% CL\n";
  return true;
}

std::pair<double, double> Hybrid::eval(RooRealVar *r, double rVal, RooStats::HybridCalculatorOriginal *hc, bool adaptive, double clsTarget) {
    using namespace RooStats;
    RooFit::MsgLevel globalKill = RooMsgService::instance().globalKillBelow();
    RooMsgService::instance().setGlobalKillBelow(RooFit::WARNING);

    r->setVal(rVal);
    std::auto_ptr<HybridResult> hcResult(hc->GetHypoTest());
    if (hcResult.get() == 0) {
        std::cerr << "Hypotest failed" << std::endl;
        RooMsgService::instance().setGlobalKillBelow(globalKill);
        return std::pair<double, double>(-1,-1);
    }
    double clsMid    = (CLs_ ? hcResult->CLs()      : hcResult->CLsplusb());
    double clsMidErr = (CLs_ ? hcResult->CLsError() : hcResult->CLsplusbError());
    std::cout << "r = " << rVal << (CLs_ ? ": CLs = " : ": CLsplusb = ") << clsMid << " +/- " << clsMidErr << std::endl;
    if (adaptive) {
        while (fabs(clsMid-clsTarget) < 3*clsMidErr && clsMidErr >= clsAccuracy_) {
            std::auto_ptr<HybridResult> more(hc->GetHypoTest());
            hcResult->Add(more.get());
            clsMid    = (CLs_ ? hcResult->CLs()      : hcResult->CLsplusb());
            clsMidErr = (CLs_ ? hcResult->CLsError() : hcResult->CLsplusbError());
            std::cout << "r = " << rVal << (CLs_ ? ": CLs = " : ": CLsplusb = ") << clsMid << " +/- " << clsMidErr << std::endl;
        }
    }
    if (verbose > 0) {
        std::cout << "r = " << r->getVal() << ": \n" <<
            "\tCLs      = " << hcResult->CLs()      << " +/- " << hcResult->CLsError()      << "\n" <<
            "\tCLb      = " << hcResult->CLb()      << " +/- " << hcResult->CLbError()      << "\n" <<
            "\tCLsplusb = " << hcResult->CLsplusb() << " +/- " << hcResult->CLsplusbError() << "\n" <<
            std::endl;
    }
    RooMsgService::instance().setGlobalKillBelow(globalKill);
    return std::pair<double, double>(clsMid, clsMidErr);
} 

