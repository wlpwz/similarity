#include <fstream>
#include "model.h"
#include "log.h"
#include "encoding.h"
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/gzip_stream.h>
#include "interface/lda.pb.h"


using namespace std;
using namespace sm;

/* extern functions is from lda_utils.c */
extern double alhood(double a, double ss, int D, int K);
extern double d_alhood(double a, double ss, int D, int K);
extern double d2_alhood(double a, int D, int K);
extern double log_sum(double log_a, double log_b);
extern double trigamma(double x);
extern double digamma(double x);
extern double log_gamma(double x);
extern int argmax(double* x, int n);
extern uint32_t randomMT();

#define myrand() (double) (((unsigned long) randomMT()) / 4294967296.)


LDAModel::LDAModel (Corpus *corpus, Dictionary *dict, int version) :
  TopicModel(corpus, dict),
  _alpha(0.01), 
  _init_alpha(1.0),
  _estimate_alpha(1), //TODO 1 or 0
  _var_max_iter(20),
  _var_converged(1e-6),
  _em_max_iter(100),
  _em_converged(1e-4),
  _max_alpha_iter(1000),
  _newton_threshold(1e-10),
  _ntopics(100),
  _ndocs(_corpus->size()),
  _version(version),
  _nterms(corpus->getNTerms()),
  _log_prob_w(NULL),
  _var_gamma(NULL),
  _phi(NULL)
{
  assert (corpus);
  assert (corpus->size() > 0);
  _init_prob();
}


void
LDAModel::_init_prob(){
  _log_prob_w = (double **) malloc (sizeof (double *) * _ntopics);
  
  for (int i = 0; i < _ntopics; i++) {
    _log_prob_w[i] = (double *) malloc (sizeof (double) * _nterms);
    for (int j = 0; j < _nterms; j++) {
      _log_prob_w[i][j] = 0.0;
    } 
  }

  // allocate variational parameters
  _var_gamma = (double **) malloc(sizeof(double*) * (_corpus->size()));
  for (size_t i = 0; i < _corpus->size(); i++)
    _var_gamma[i] = (double *) malloc(sizeof(double) * _ntopics);

  int max_length = _corpus->maxDocLen();
  _phi = (double **) malloc( sizeof(double*) * max_length);
  for (int i = 0; i < max_length; i++)
    _phi[i] = (double *) malloc(sizeof(double) * _ntopics);
}


LDAModel::~LDAModel(){

  if (_var_gamma) {
    for (size_t i = 0; i < _corpus->size(); i++) free (_var_gamma[i]);
    free (_var_gamma);
  }

  if (_phi) {
    for (size_t i = 0; i < _corpus->maxDocLen(); i++) free (_phi[i]);
    free (_phi);
  }




  if (_log_prob_w){ 
    for (int i = 0; i < _ntopics; i++) {
      free (_log_prob_w[i]);
    }
    free (_log_prob_w);
  }
}


int
LDAModel::_cluster(){
  _topics.resize(_ntopics);

  SM_LOG_DEBUG ("begin cluster docs %zu", _topics.size());

  for (size_t i = 0; i < _corpus->size(); i++) {
    bow_t ret;
    getMostLikelyTopicOfDoc (&ret, i, 0, 1);
    if (ret.size() != 0) {
      SM_LOG_DEBUG ("doc [%zu] belongs to topic [%d:%lf]", i, ret[0].id, ret[0].weight);
      assert (ret[0].id >= 0 && ret[0].id < _ntopics);
      _topics[ret[0].id].push_back (i);
    } else {
      SM_LOG_WARNING ("wtf");
    }
  }

  return 0;
}

int 
LDAModel::train(){
  // initialize model
  LDAState *ss = new LDAState(*_corpus, _ntopics);
  _mle(ss, 0);
  _alpha = _init_alpha;

  _em (ss);

  return 0;
}


void
LDAModel::_em(LDAState *ss){
  size_t d;
  int i;
  double likelihood, likelihood_old = 0, converged = 1;

  // run expectation maximization
  i = 0; 
  likelihood = likelihood_old = 0.0;
  converged = 1;
  while (((converged < 0) || (converged > _em_converged) || (i <= 2)) && (i <= _em_max_iter)) {
    i++; 
    SM_LOG_DEBUG("**** em iteration %d ****", i);
    likelihood = 0;
    
    ss->zero ();
    // e-step
    for (d = 0; d < _corpus->size(); d++) {
      likelihood += _e_step(_corpus->at(d),
                            _var_gamma[d],
                            _phi,
                            ss);
    }

    // m-step
    _mle(ss, _estimate_alpha);

    // check for convergence
    converged = (likelihood_old - likelihood) / (likelihood_old);
    if (converged < 0) _var_max_iter = _var_max_iter * 2;
    likelihood_old = likelihood;
  }

}


int 
LDAModel::inference (const bow_t &src, bow_t *ret, bool normalized){
  assert (0); //TODO, implement this
  assert (ret->size() == 0);
  double *var_gamma[1], **phi;

  var_gamma[0] =  (double *)malloc (sizeof (double) * _ntopics);
  phi = (double **) malloc (sizeof (double *) * src.size());
  for (size_t n = 0; n < src.size(); n++) {
    phi[n] = (double *) malloc (sizeof (double) * _ntopics);
  }
  _infer (src, var_gamma[0], phi);

  if (normalized) {
    ret->unitvec();
  }
  return 0;
}


int LDAModel::inference (const Corpus& __attribute__((unused)), 
                         Corpus *__attribute__((unused)), 
                         bool __attribute__((unused))) {
  return 0;
}

int LDAModel::save (const std::string &path, const std::string &basename) {
  char filename[PATH_MAX];
  if (_version != 0) {
    snprintf (filename, PATH_MAX, "%s/%s.lda.%lu", path.c_str(), basename.c_str(), _version);
  } else {
    snprintf (filename, PATH_MAX, "%s/%s.lda", path.c_str(), basename.c_str());
  }
  
  ofstream os(filename);
  if (!os.is_open()){
    SM_LOG_WARNING ("open %s error store lda", filename);
    return -1;
  }
  google::protobuf::io::OstreamOutputStream oos(&os);
  google::protobuf::io::GzipOutputStream gzips(&oos);
  
  smpb::LDA slda;

  slda.set_version(_version);
  slda.set_topics(_ntopics);
  slda.set_terms(_nterms);
  slda.set_alpha(_alpha);

  for (int i = 0; i < _ntopics; i++) {
    for (int j = 0; j < _nterms; j++) {
      slda.add_beta(_log_prob_w[i][j]);
    }
  }
  
  for (size_t i = 0; i < _ndocs; i++){
    for (size_t j = 0; j < _ntopics; j++) {
      slda.add_gamma(_var_gamma[i][j]);
    }
  }

  if (!slda.SerializeToZeroCopyStream(&gzips)){
    SM_LOG_WARNING ("serialize to %s error", filename);
    return -1;
  }

  SM_LOG_NOTICE ("save model %s success", filename);
  return 0;
}


int 
LDAModel::load (const std::string &path, const std::string &basename) {
  char filename[PATH_MAX];

  if (_version != 0) {
    snprintf (filename, PATH_MAX, "%s/%s.lda.%lu", path.c_str(), basename.c_str(), _version);
  } else {
    snprintf (filename, PATH_MAX, "%s/%s.lda", path.c_str(), basename.c_str(), _version);
  }

  ifstream is(filename);
  if (!is.is_open()) {
    SM_LOG_WARNING ("open dict file %s error", filename);
    return -1;
  }

  google::protobuf::io::IstreamInputStream iis(&is);
  google::protobuf::io::GzipInputStream gzips(&iis);

  smpb::LDA dlda;
  if (!dlda.ParseFromZeroCopyStream(&gzips)) {
    SM_LOG_WARNING ("parse lda model [%s] error", filename);
    return -1;
  }

  if (_version != dlda.version()) {
    SM_LOG_WARNING ("expect lda model version is %lu, file is %lu", 
                    _version, dlda.version());
    return -1;
  }


  _nterms = dlda.terms();
  _ntopics = dlda.topics();
  _ndocs = dlda.docs();
  _init_prob();

  for (int i = 0; i < _ntopics; i++) {
    for (int j = 0; j < _nterms; j++) {
      _log_prob_w[i][j] = dlda.beta(i*_nterms + j);
    }
  }

  for (size_t i = 0; i < _ndocs; i++) {
    for (int j = 0; j < _ntopics; j++) {
      _var_gamma[i][j] = dlda.gamma(i*_ntopics+j);
    }
  }

  SM_LOG_NOTICE ("load lda model [%s] success", filename);

  _cluster();
  return 0;
}


int
LDAModel::getMostLikelyTopicOfDoc (bow_t *ret, int docid, double threshold, int max_result) {
  assert (ret && ret->size() == 0);
  assert (docid < (int)_corpus->size());

  for (int i = 0; i < _ntopics; i++) {
    bow_unit_t u;
    u.id = i;
    u.weight = _var_gamma[docid][i];
    if (u.weight < threshold)
      continue;

    ret->push_topk (u, max_result);
  }

  ret->sort();
  return 0;
}


int
LDAModel::getDocsOfTopic (vector<int> *tmp, int topicid) {
  assert (topicid <= _ntopics);

  *tmp =  _topics[topicid-1];
  return 0;

}


void
LDAModel::getHotestWords (bow_t *bow, int topicid, int nwords) {
  assert (topicid >=1 && topicid <= _ntopics);
  assert (bow->size() == 0);

  bow_unit_t tmp;
  int i;

  for (i = 0; i < _nterms; i++) {
    tmp.id = i;
    tmp.weight = _log_prob_w[topicid-1][i];
    bow->push_topk (tmp, nwords);
  }
  bow->sort();
}

void
LDAModel::getHotestWordsDesc(string *desc, int topicid, int nwords, const std::string& encoding){
  assert (desc->size() == 0);
  bow_t bow;
  getHotestWords(&bow, topicid, nwords);
  
  stringstream ss;
  rwtrans_func_t *w = get_rwtrans(encoding);
  assert(w);

  ss << "Topic [" << topicid-1 << "]: ";

  for (size_t i = 0; i < bow.size(); i++) {
    string buffer;
    if (_dict) {
      assert (bow[i].id < _dict->size());
      const wstring &d = _dict->at(bow[i].id);
      assert (0 == w (d, &buffer) );
      ss << buffer;
    } else {
      ss << bow[i].id;
    }
    ss << "*" << bow[i].weight << " ";
  }
  
  desc->assign (ss.str());
}


double 
LDAModel::_e_step(const bow_t& doc, 
                  double* gamma, 
                  double** phi,
                  LDAState* ss)
{
    double likelihood;
    size_t n;
    int k;

    // posterior inference
    likelihood = _infer(doc, gamma, phi);
    // update sufficient statistics
    double gamma_sum = 0;
    for (k = 0; k < _ntopics; k++) {
        gamma_sum += gamma[k];
        ss->alpha_suffstats += digamma(gamma[k]);
    }
    ss->alpha_suffstats -= _ntopics * digamma(gamma_sum);

    for (n = 0; n < doc.size(); n++) {
        for (k = 0; k < _ntopics; k++) {
          ss->class_word[k][doc[n].id] += doc[n].weight*phi[n][k];
          ss->class_total[k] += doc[n].weight*phi[n][k];
        }
    }

    ss->ndocs +=  1;
    return likelihood;
}


void
LDAModel::_mle (LDAState* ss, int estimate_alpha) {
  int k; int w;

  for (k = 0; k < _ntopics; k++) {
    for (w = 0; w < _nterms; w++) {
      if (ss->class_word[k][w] > 0) {
        _log_prob_w[k][w] =
          log(ss->class_word[k][w]) -
          log(ss->class_total[k]);
      } else {
        _log_prob_w[k][w] = -100;
      }
    }
  }

  if (estimate_alpha == 1) {
    _alpha = _opt_alpha(ss->alpha_suffstats,
                        ss->ndocs,
                        _ntopics);

    SM_LOG_DEBUG ("new alpha = %5.5f", _alpha);
  }
}

double 
LDAModel::_infer(const bow_t& doc, double* var_gamma, double** phi) {
    double converged = 1;
    double phisum = 0, likelihood = 0;
    double likelihood_old = 0, oldphi[_ntopics];
    size_t n;
    int k;
    int var_iter;
    double digamma_gam[_ntopics];

    // compute posterior dirichlet
    for (k = 0; k < _ntopics; k++) {
      assert (doc.total() != NAN);
      var_gamma[k] = _alpha + (doc.total()/((double) _ntopics));
      digamma_gam[k] = digamma(var_gamma[k]);
      for (n = 0; n < doc.size(); n++)
        phi[n][k] = 1.0/_ntopics;
    }
    var_iter = 0;

    while ((converged > _var_converged) &&
           ((var_iter < _var_max_iter) || (_var_max_iter == -1))) {
	var_iter++;
	for (n = 0; n < doc.size(); n++) {
            phisum = 0;
            for (k = 0; k < _ntopics; k++) {
              oldphi[k] = phi[n][k];
              phi[n][k] =
                digamma_gam[k] +
                _log_prob_w[k][doc[n].id];

              if (k > 0)
                phisum = log_sum(phisum, phi[n][k]);
              else
                phisum = phi[n][k]; // note, phi is in log space
            }

            for (k = 0; k < _ntopics; k++) {
              phi[n][k] = exp(phi[n][k] - phisum);
              var_gamma[k] =
                var_gamma[k] + doc[n].weight*(phi[n][k] - oldphi[k]);
              // !!! a lot of extra digamma's here because of how we're computing it
              // !!! but its more automatically updated too.
              digamma_gam[k] = digamma(var_gamma[k]);
            }
        }

        likelihood = _compute_likelihood(doc, phi, var_gamma);
        assert(!isnan(likelihood));
        converged = (likelihood_old - likelihood) / likelihood_old;
        likelihood_old = likelihood;

        //SM_LOG_TRACE ("[LDA INF] %8.5f %1.3e", likelihood, converged);
    }
    return likelihood;
}


double 
LDAModel::_opt_alpha(double ss, int D, int K) {
  double a, log_a, init_a = 100;
  double f, df, d2f;
  int iter = 0;

  log_a = log(init_a);
  do {
    iter++;
    a = exp(log_a);
    if (isnan(a)) {
      init_a = init_a * 10;
      SM_LOG_DEBUG ("warning : alpha is nan; new init = %5.5f", init_a);
      a = init_a;
      log_a = log(a);
    }
    f = alhood(a, ss, D, K);
    df = d_alhood(a, ss, D, K);
    d2f = d2_alhood(a, D, K);
    log_a = log_a - df/(d2f * a + df);
    SM_LOG_DEBUG ("alpha maximization : %5.5f   %5.5f", f, df);
  }
  while ((fabs(df) > _newton_threshold) && (iter < _max_alpha_iter));
  return exp(log_a) ;
}



double
LDAModel::_compute_likelihood(const bow_t& doc, double** phi, double* var_gamma) {
  double likelihood = 0, digsum = 0, var_gamma_sum = 0, dig[_ntopics];
  int k;
  size_t n;

  for (k = 0; k < _ntopics; k++) {
    dig[k] = digamma(var_gamma[k]);
    var_gamma_sum += var_gamma[k];
  }
  digsum = digamma(var_gamma_sum);

  likelihood =
    lgamma(_alpha * _ntopics)
    - _ntopics * lgamma(_alpha)
    - (lgamma(var_gamma_sum));

  for (k = 0; k < _ntopics; k++) {
    likelihood +=
      (_alpha - 1)*(dig[k] - digsum) + lgamma(var_gamma[k])
      - (var_gamma[k] - 1)*(dig[k] - digsum);

    for (n = 0; n < doc.size(); n++) {
      if (phi[n][k] > 0) {
        likelihood += doc[n].weight*
          (phi[n][k]*((dig[k] - digsum) - log(phi[n][k])
                      + _log_prob_w[k][doc[n].id]));
      }
    }
  }
  return likelihood;
}


LDAState::LDAState(const Corpus &corpus, int topics):
  ndocs(0),
  _nterms (corpus.getNTerms()),
  _ntopics(topics)
{
  int i, j, k, n;

  class_word =  (double **)  malloc (sizeof (double) * _ntopics);
  class_total = (double *)  malloc (sizeof (double*) * _ntopics);


  for (i = 0; i < _ntopics; i++) {
    class_total[i] = 0.0;
    class_word[i] = (double *) malloc (sizeof (double) * _nterms);
    for (j = 0; j < _nterms; j++) {
      class_word[i][j] = 0.0;
    }
  }

  /*
  for (k = 0; k < _ntopics; k++) {
    for (i = 0; i < num_init; i++) {
      d = floor(myrand() * corpus.size());
      SM_LOG_DEBUG ("initialized with document %d", d);
      const bow_t& doc = corpus[d];
      for (n = 0; n < doc.size(); n++) {
        class_word[k][doc[n].id] += doc[n].weight;
      }
    }

    for (n = 0; n < _nterms; n++) {
      class_word[k][n] += 1.0;
      class_total[k] = class_total[k] + class_word[k][n];
    }
    }*/

  for (k = 0; k < _ntopics; k++)
    for (n = 0; n < _nterms; n++) {
      class_word[k][n] += 1.0/_nterms + myrand();
      class_total[k] += class_word[k][n];
    }
}

LDAState::~LDAState(){
  for (int i = 0; i < _ntopics; i++) {
    free(class_word[i]);
  }

  free(class_total);
  free(class_word);
}


void
LDAState::zero() {
  int k, w;
  for (k = 0; k < _ntopics; k++) {
    class_total[k] = 0;
    for (w = 0; w < _nterms; w++) {
      class_word[k][w] = 0;
    }
  }

  ndocs = 0;
  alpha_suffstats = 0;
}
