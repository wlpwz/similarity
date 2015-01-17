#ifndef _SM_REPO_H_
#define _SM_REPO_H_

#include <vector>
#include <string>

#include <sys/types.h>
#include <map>
#include "concurrent.h"
#include "dictionary.h"
#include "kvproxy_client.h"
#include "corpus.h"

namespace sm {
  class Repository;
  typedef int (ItemCallback)(const std::string &item, const std::string& content, void *context);

  class RepositoryWorker : public Thread<std::string> {
  public:
    RepositoryWorker(Repository *repo);
    virtual ~RepositoryWorker();
    virtual int doJob(const std::string &job);
  private:
    int _read_from_local(uint64_t docid, std::string* content);
    int _save_to_local(uint64_t docid, const std::string& content);
    Repository* _repo;
    KvProxyClient client;
  };

  class Repository {
  public:
    Repository(int nworkers =12, const std::string &local = "");
    virtual ~Repository();

    int addUrl (const std::string& url);
    int addUrls (const std::vector<std::string> &urls);
    int registerItemHandler(ItemCallback);
    int size();
    int doJob(const std::string &doc);
    
    void waitAllJobDone();
    
    int tfidf ();
    int lda();

    int save(const std::string &basepath);

    Corpus& corpus(){ return _corpus; }
    Dictionary& dict(){ return _dict; }
    
    const std::string getLocalCachePath(){ return _localpath; }
   private:
    std::string _localpath;
    std::map<int, uint64_t> _docmap;

    std::vector<RepositoryWorker *> _workers;
    Lock _docmapLock;
    Dictionary _dict;
    Corpus _corpus, _tfidf;
  };
};
#endif