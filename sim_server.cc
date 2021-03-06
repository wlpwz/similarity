#include <inttypes.h>
#include "json/json.h"
#include "sim_server.h"
#include "log.h"
#include "mola_wrapper.h"
#include "kvproxy_client.h"
#include "redis_wrapper.h"
#include "configurable.h"
#include "interface/similarity.pb.h"


using namespace baidu::mco;
using namespace sm;
using namespace std;

/*
const static int ERROR_CODE_##message = -code;          \
const static char *ERROR_MSG_##message = #message;   \
*/

#define DEF_ERROR(code, message)                         \
  const static char *ERROR_JSON_##message = "{ \"error_code\" : -" #code ", \"error_message\" : \"" #message " \"}";

DEF_ERROR(1, FORMAT)
DEF_ERROR(2, DOCID_NOT_SET)
DEF_ERROR(3, DOCID)
DEF_ERROR(4, RESULT)
DEF_ERROR(5, THRESHOLD)
DEF_ERROR(10, INTERNAL)

typedef ub::SmartEvent<SimServerEvent> SimServerEventPtr;

SimServer::SimServer(ub::NetReactor *reactor):
  ub::UbAServer (reactor), _server_data(NULL)
{
  SM_CONFIG_BEGIN(cache)
  SM_CONFIG_PROP(cache_expire, int32, 60); //default one day expire
  SM_CONFIG_END
}

SimServer::~SimServer(){
  
}

int
SimServer::updateServerData(SimServerData *data) {
  int ret;
  _dataLock.AcquireWrite();
  if (_server_data == NULL)  {
    if ( 0 != this->getready() ){
      SM_LOG_FATAL("server start error");
      ret = -1;
    } else {
      _server_data = data;
      SM_LOG_NOTICE ("data has upgraded to version : %" PRIu64, data->getVersion());
    }
  }
  _dataLock.Release();

  return ret;
}

int
SimServer::getSimilarities (sim_t *sims, uint64_t docid, float threshold, int max_result) {
  int ret;
  if (0 == seek_from_cache(sims, docid, threshold, max_result)) {
    return 0;
  }
  _dataLock.AcquireRead();
  ret = _server_data->getSimilarity (sims, docid, threshold, max_result);
  _dataLock.Release();
  if (sims->size() != 0)
    save_to_cache(*sims, docid);
  return ret;
}


void
SimServer::on_accept (ub::UbEvent *event) {
  SimServerEventPtr mev;
  session_begin(&mev);
}


void
SimServerEvent::http_read_header_done(){

}


void
SimServerEvent::http_read_section_done(){

}


void
SimServerEvent::session_begin (){
  http_package_read ();
}


void
SimServerEvent::read_done_callback (){
  if (isError()) {
    SM_LOG_WARNING ("http package error");
    return;
  }

  const void *req_buf = this->get_read_buffer();
  int headlen = this->get_http_headlen();
  int bodylen = this->get_http_bodylen();
  sim_t sims;
  SimServer *server = (SimServer *) _fserver;
  uint64_t docid;
  float threshold = server->getDefaultThreshold();
  int max_result = server->getDefaultMaxResult();
  string ret;
  char resheader[1024];
  
  string request((char *)req_buf+headlen, bodylen);
  if (0 == _get_request(request, &docid, &threshold, &max_result, &ret)) {
    if (0 != server->getSimilarities (&sims, docid, threshold, max_result)){
      SM_LOG_NOTICE("get similarity fail [%" PRIu64 "]", docid );
      ret = ERROR_JSON_INTERNAL;
    } else {
      _sims_to_json(sims, &ret);
    }
  }

  int res_header_len;

  res_header_len = snprintf (resheader, 1024, 
                             "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                             "Content-Length: %zu\r\n\r\n", ret.size());

  void *res_buf = this->get_write_buffer(res_header_len+ret.size());

  memcpy (res_buf, resheader, res_header_len);
  memcpy ((char *)res_buf+res_header_len, ret.data(), ret.size());
  awrite(res_header_len + ret.size());
}

void
SimServerEvent::event_error_callback (){
  UBEVENT_TRACE(this,"SimServerEvent(read_done_callback) event error(0x%X)[%s]", this->get_sock_status(),
                status_to_string(get_sock_status()).c_str());
  _fserver->session_done(this);
  return;
}


SM_IMP_SINGLETON (SimServerDataManager)

SimServerDataManager::~SimServerDataManager(){
  for (map<uint64_t, SimServerData *>::iterator iter = _datas.begin();
       iter != _datas.end();
       iter++)
    {
      delete iter->second;
    }
    
}

int
SimServerDataManager::init () {
  _local_version = 0; //TODO check dir

  return 0;
}


int
SimServerDataManager::checkVersion(){
  uint64_t remote_version = 0; // TODO get version from remote

  if (remote_version >= _local_version) {
    SM_LOG_NOTICE ("check trainer version %" PRIu64 " : localversion is %" PRIu64
                   , remote_version, _local_version);
    SimServerData *data;
    if (_datas.find(remote_version) != _datas.end()) {
      data = _datas[remote_version];
    } else {
      data = new SimServerData(remote_version);    
      if (0 != data->load()){
        delete data;
        return -1;
      }
      _datas[remote_version] = data;
    }

    if (0 != _server->updateServerData(data) ){
      SM_LOG_WARNING ("update server data error!, can't bind new port?");
      return -1;
    }
    _local_version = remote_version;
  }

  return 0;

}


int
SimServerDataManager::registerSimServer(SimServer *server) {
  _server = server;
  return 0;
}


SimServerData::SimServerData(uint64_t version):
  _version (version), _corpus(NULL), _dict(NULL) ,_model(NULL), _sim(NULL)
{

}

SimServerData::~SimServerData(){
  if (_dict) delete _dict;
  if (_corpus) delete _corpus;
  if (_model) delete _model;
}


int
SimServerData::getSimilarity(sim_t *sims, uint64_t docid, float threshold, int max_result){
  SM_ASSERT (sims && sims->size()==0, "ret can't be empty");
  bow_t sim_bow;
  int id = _corpus->getIdFromDocid (docid);
  if (-1 != id) {
    if ( 0 != _sim->getSimilarities(&sim_bow, id, threshold, max_result) ){
      return -1;
    }
  } else {
    SM_LOG_DEBUG ("Getting an unseen doc [%" PRIu64 "]", docid);
    std::string url;
    KvProxyClient client;
    string content;
    bow_t topic_bow, doc_bow;
    Document doc;

    if (0 != getUrlFromDocid (docid, &url)){
      return -1;
    }

    if (0 != client.get(url+"|topic", &content)) {
      return -1;
    }


    if (0 != doc.parseFromJsonRaw(content)){
      return -1;
    }

    if (0 != doc.analysis()) {
      return -1;
    }

    _dict->doc2bow(&doc_bow, doc, false);
    doc_bow.pre_handle();
    if (doc_bow.size() == 0) {
      SM_LOG_NOTICE ("docid [%" PRIu64 "]bow is zero", doc.getId());
      return -1;
    }
    //TODO tfidf?

    if (0 != _model->inference(doc_bow, &topic_bow)){
      return -1;
    }


    SM_LOG_DEBUG ("most likely topic [%d:%.3lf]", topic_bow[0].id, topic_bow[0].weight);
    vector<int> docInTopic;
    _model->getDocsOfTopic (&docInTopic, topic_bow[0].id);

#ifdef DEBUG
    cout << "Topic Liklihood: ";
    for (size_t i = 0; i < topic_bow.size(); i++) {
      cout << "[" << topic_bow[i].id << ":" << topic_bow[i].weight << "] ";
    }
    cout << endl;

    cout << "Document in Topic [" << topic_bow[0].id <<  "]" << endl;
    for (vector <int>::iterator iter = docInTopic.begin();
         iter != docInTopic.end();
         iter++)
      cout << _corpus->getDocid(*iter) << endl;
    cout << endl;
#endif
    
    if (0 != _sim->getSimilarities (&sim_bow, doc_bow, docInTopic, threshold, max_result)) {
      return -1;
    }

  }

  for (size_t i = 0; i < sim_bow.size(); i++) {
    sim_unit_t u;
    u.docid = _corpus->getDocid(sim_bow[i].id);
    u.sim = sim_bow[i].weight;
    sims->push_back(u);
  }

  return 0;
}


uint64_t
SimServerData::getVersion(){
  return _version;
}


int
SimServerData::load() {
  _dict = new Dictionary(_version);
  if ( 0 != _dict->load("similarity")){
    SM_LOG_DEBUG ("load dict error");
    return -1;
  }
  
  _corpus = new Corpus(_dict, _version);
  if ( 0 != _corpus->load("similarity")){
    SM_LOG_DEBUG ( "load corpus error");
    return -1;
  }

  _model = new LDAModel(_corpus, _dict);
  if ( 0 != _model->load()){
    SM_LOG_DEBUG( "load lda error");
    return -1;
  }

  _sim = new TopicSimilarity (_model, _corpus, _dict);

  if (0 != _sim->load ()) {
    SM_LOG_WARNING ("load sim model error");
    return -1;
  }
  
  SM_LOG_NOTICE ("new sim version %" PRIu64 "  loaded", _version);


#include <iostream>
  for (size_t i = 0; i < _corpus->size(); i++) {
    bow_t retbow;
    cout << "Sim of [ " << _corpus->getDocid(i)  << ":";
    _sim->getSimilarities(&retbow, i, 0.0, 5);
    cout << retbow.size() << "]\t";

    for (size_t j = 0; j < retbow.size(); j++) {
      cout << "[" << _corpus->getDocid(retbow[j].id) << ":" << retbow[j].weight << "]";
    }
    cout << endl;
  }

  return 0;
}


int 
SimServerEvent::_get_request(const string &request, 
                             uint64_t* docid, float* threshold, int* max_result, string *err) {
  Json::Reader reader;
  Json::Value value;
  if (!reader.parse(request, value, false)) {
    (*err) = ERROR_JSON_FORMAT;
    return -1;
  }

  if (!value.isMember ("docid") ) {
    (*err) = ERROR_JSON_DOCID_NOT_SET;
    return -1;
  }
  
  if (value["docid"].isNumeric()) {
    *docid = value["docid"].asUInt64();
  } else if (value["docid"].isString()) {
    int ret = sscanf (value["docid"].asString().c_str(), "%" PRIu64, docid); //support php string type
    if (ret != 1) {
      (*err) = ERROR_JSON_DOCID;
      return -1;
    }
  } else {
    (*err) = ERROR_JSON_DOCID;
    return -1;
  }


  if (value.isMember ("filter")) {
    if (value["filter"].isMember("threshold")) {
      if (!value["filter"]["threshold"].isNumeric()) {
        (*err) = ERROR_JSON_THRESHOLD;
        return -1;
      } else {
        *threshold = value["filter"]["threshold"].asFloat();
      }
    }

    if (value["filter"].isMember("max_result")) {
      if (!value["filter"]["max_result"].isInt()) {
        (*err) = ERROR_JSON_RESULT;
        return -1;
      } else {
        *max_result = value["filter"]["max_result"].asInt();
      }
    }
  }

  SM_LOG_NOTICE ("[REQUEST] [docid:%" PRIu64 "] threshold: %lf max_result:%d",
                 *docid, *threshold, *max_result);
  return 0;
}


void 
SimServerEvent::_sims_to_json(const sim_t &sims, string *strret) {
  Json::Value ret(Json::objectValue), simarr(Json::arrayValue);
  ret["error_code"] = 0;
  ret["error_message"] = "ok";

   for (size_t i = 0; i < sims.size(); i++) {
     Json::Value sim(Json::objectValue);

     Json::Value simv(sims[i].sim);
     std::stringstream ss;
     ss << sims[i].docid;
     Json::Value docidv(ss.str());

     sim["docid"] = docidv;
     sim["similarity"] = simv;
     simarr.append(sim);
  }
   ret["similarities"] = simarr;

   Json::FastWriter writer;
   *strret = writer.write(ret);
}

static void
_gen_sim_key(uint64_t docid, std::string *keybuf) {
  SM_ASSERT (keybuf && keybuf->size() == 0, "key buf should be empty");
  char mybuf[128];
  int ret = snprintf (mybuf, 128, "__SIMILARITY__%" PRIu64, docid);
  keybuf->assign(mybuf, ret);
}


int
SimServer::seek_from_cache(sim_t *sims, uint64_t docid, float threshold, int max_result){
  SM_ASSERT(sims && sims->size() == 0, "get a wrong para");
  RedisEngine& engine = RedisEngineManager::getInstance()->getEngine();

  string mykey;
  _gen_sim_key(docid, &mykey);
  Slice key(mykey);
  Slice value;
  
  if (engine.get(key, value) != 0) {
    SM_LOG_WARNING ("get key %" PRIu64 " from redis error", docid);
    return -1;
  }
  if (value.size() == 0) {
    return -1;
  }

  SM_LOG_DEBUG ("[LOAD_CACHE] key : %s value size : %zu", key.data(), value.size());

  smpb::Sim dsim;
  string ss(value.data(), value.size());
  if (!dsim.ParseFromString(ss)) {
    SM_LOG_WARNING("get key %" PRIu64 "but invalid format", docid);
    return -1;
  }

  
  for (int i = 0; i < dsim.units_size(); i++) {
    if (i >= max_result) break;

    if (dsim.units(i).sim() < threshold) {
      break;
    }
    
    sim_unit_t u;
    u.docid = dsim.units(i).id();
    u.sim = dsim.units(i).sim();
    sims->push_back(u);
  }

  return 0;
}


int
SimServer::save_to_cache(const sim_t& sim ,uint64_t docid) {
  smpb::Sim ssim;
  string mykey;
  _gen_sim_key(docid, &mykey);
  RedisEngine &engine = RedisEngineManager::getInstance()->getEngine();
  Slice key(mykey);

  for (size_t i = 0; i < sim.size(); i++) {
    smpb::SimUnit *sunit = ssim.add_units();
    sunit->set_id(sim[i].docid);
    sunit->set_sim(sim[i].sim);
  }

  string buffer;
  if (!ssim.SerializeToString(&buffer)) {
    SM_LOG_WARNING ("serialize to buffer error");
    return -1;
  }

  SM_LOG_DEBUG ("[SAVE_CHCHE] key : %s value size : %zu", key.data(), buffer.size());

  Slice value(buffer);
  if (engine.set(key, value) != 0) {
    SM_LOG_WARNING ("set to redis error");
    return -1;
  }

  if (engine.expire(key, _cache_expire) != 0) {
    SM_LOG_WARNING ("expire redis key error");
    return -1;
  }

  return 0;
}


#undef DEF_ERROR
