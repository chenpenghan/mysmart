#include "Timer.h"
#include "Tree.h"
#include "zipf.h"

#include <city.h>
#include <stdlib.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <random>

//////////////////// workload parameters /////////////////////

// #define USE_CORO
// #define waiting_test
const int kCoroCnt = 10;

int kReadRatio;
int kThreadCount;
int kNodeCount;
uint64_t kKeySpace = 4 * define::MB;
double kWarmRatio = 0.5;
double zipfan = 0.99;

//////////////////// workload parameters /////////////////////


extern double cache_miss[MAX_APP_THREAD];
extern double cache_hit[MAX_APP_THREAD];



std::thread th[MAX_APP_THREAD];
uint64_t tp[MAX_APP_THREAD][8];

extern uint64_t latency[MAX_APP_THREAD][LATENCY_WINDOWS];
uint64_t latency_th_all[LATENCY_WINDOWS];

Tree *tree;
DSM *dsm;

class RequsetGenBench : public RequstGen {

public:
  RequsetGenBench(int coro_id, DSM *dsm, int id)
      : coro_id(coro_id), dsm(dsm), id(id) {
    seed = rdtsc();
    mehcached_zipf_init(&state, kKeySpace, zipfan,
                        (rdtsc() & (0x0000ffffffffffffull)) ^ id);
  }

  Request next() override {
    Request r;
    uint64_t dis = mehcached_zipf_next(&state);

    r.k = int2key(dis);
    r.v = 23;
    r.is_search = rand_r(&seed) % 100 < kReadRatio;

    tp[id][0]++;

    return r;
  }

private:
  int coro_id;
  DSM *dsm;
  int id;

  unsigned int seed;
  struct zipf_gen_state state;
};

RequstGen *coro_func(int coro_id, DSM *dsm, int id) {
  return new RequsetGenBench(coro_id, dsm, id);
}

Timer bench_timer;
std::atomic<int64_t> warmup_cnt{0};
std::atomic_bool ready{false};
void thread_run(int id) {

  bindCore(id);

  dsm->registerThread();

  uint64_t all_thread = kThreadCount * dsm->getClusterSize();
  uint64_t my_id = kThreadCount * dsm->getMyNodeID() + id;

  printf("I am thread %ld on compute nodes\n", my_id);

  if (id == 0) {
    bench_timer.begin();
  }

  uint64_t end_warm_key = kWarmRatio * kKeySpace;
  // uint64_t end_warm_key = 30000000;
  for (uint64_t i = 1; i < end_warm_key; ++i) {
    if (i % all_thread == my_id) {
      tree->insert(int2key(i), i * 2);
    }
  }

  warmup_cnt.fetch_add(1);

  if (id == 0) {
    while (warmup_cnt.load() != kThreadCount)
      ;
    printf("node %d finish\n", dsm->getMyNodeID());
    dsm->barrier("warm_finish");

    uint64_t ns = bench_timer.end();
    printf("warmup time %lds\n", ns / 1000 / 1000 / 1000);

    // tree->index_cache_statistics();
    // tree->clear_statistics();

    ready = true;

    warmup_cnt.store(0);
  }

  while (warmup_cnt.load() != 0)
    ;

#ifdef USE_CORO
  tree->run_coroutine(coro_func, id, kCoroCnt);
#else

  /// without coro
  unsigned int seed = rdtsc();
  struct zipf_gen_state state;
  mehcached_zipf_init(&state, kKeySpace, zipfan,
                      (rdtsc() & (0x0000ffffffffffffull)) ^ id);

  // 创建随机数生成器引擎
  std::random_device rd;  // 用于种子引擎
  std::mt19937 gen(rd()); // 使用 Mersenne Twister 引擎    
  // 定义随机数分布
  std::uniform_int_distribution<int> distribution(1, 10000); // 生成 1 到 100 之间的整数

  Timer timer;
  while (true) {

    uint64_t dis = mehcached_zipf_next(&state);
    dis = dis == 0? 1: dis;
    Key key = int2key(dis);

    Value v = distribution(gen);

    timer.begin();

    if (rand_r(&seed) % 100 < kReadRatio) { // GET
      tree->search(key, v);
    } else {
      tree->insert(key, v);
    }

    auto us_10 = timer.end() / 100;
    if (us_10 >= LATENCY_WINDOWS) {
      us_10 = LATENCY_WINDOWS - 1;
    }
    latency[id][us_10]++;

    tp[id][0]++;
  }
#endif

}

void parse_args(int argc, char *argv[]) {
  if (argc != 4) {
    printf("Usage: ./benchmark kNodeCount kReadRatio kThreadCount\n");
    exit(-1);
  }

  kNodeCount = atoi(argv[1]);
  kReadRatio = atoi(argv[2]);
  kThreadCount = atoi(argv[3]);

  printf("kNodeCount %d, kReadRatio %d, kThreadCount %d\n", kNodeCount,
         kReadRatio, kThreadCount);
}

void cal_latency() {
  uint64_t all_lat = 0;
  for (int i = 0; i < LATENCY_WINDOWS; ++i) {
    latency_th_all[i] = 0;
    for (int k = 0; k < MAX_APP_THREAD; ++k) {
      latency_th_all[i] += latency[k][i];
    }
    all_lat += latency_th_all[i];
  }

  uint64_t th30 = all_lat * 3 / 10;
  uint64_t th50 = all_lat / 2;
  uint64_t th90 = all_lat * 9 / 10;
  uint64_t th95 = all_lat * 95 / 100;
  uint64_t th99 = all_lat * 99 / 100;
  uint64_t th999 = all_lat * 999 / 1000;

  uint64_t cum = 0;
  for (int i = 0; i < LATENCY_WINDOWS; ++i) {
    cum += latency_th_all[i];
    if (cum >= th30) {
      printf("p30 %f\t", i / 10.0);
      th30 = -1;
    }
    if (cum >= th50) {
      printf("p50 %f\t", i / 10.0);
      th50 = -1;
    }
    if (cum >= th90) {
      printf("p90 %f\t", i / 10.0);
      th90 = -1;
    }
    if (cum >= th95) {
      printf("p95 %f\t", i / 10.0);
      th95 = -1;
    }
    if (cum >= th99) {
      printf("p99 %f\t", i / 10.0);
      th99 = -1;
    }
    if (cum >= th999) {
      printf("p999 %f\n", i / 10.0);
      th999 = -1;
      return;
    }
  }
}

int main(int argc, char *argv[]) {

  parse_args(argc, argv);

  DSMConfig config;
  config.machineNR = kNodeCount;
  dsm = DSM::getInstance(config);

  dsm->registerThread();
  tree = new Tree(dsm);

	// 创建随机数生成器引擎
	std::random_device rd;  // 用于种子引擎
	std::mt19937 gen(rd()); // 使用 Mersenne Twister 引擎    
	// 定义随机数分布
	std::uniform_int_distribution<int> distribution(1, 1000); // 生成 1 到 100 之间的整数

	// 生成随机数
	int v = distribution(gen);


  if (dsm->getMyNodeID() == 0) {
    for (uint64_t i = 1; i < 16000; ++i) {
      tree->insert(int2key(i), v, nullptr, 0, true);
    }
  }

  dsm->barrier("benchmark");
  // dsm->resetThread();

  for (int i = 0; i < kThreadCount; i++) {
    th[i] = std::thread(thread_run, i);
  }

  while (!ready.load())
    ;

  timespec s, e;
  uint64_t pre_tp = 0;

  int count = 0;

  clock_gettime(CLOCK_REALTIME, &s);
  while (true) {

    sleep(2);
    clock_gettime(CLOCK_REALTIME, &e);
    int microseconds = (e.tv_sec - s.tv_sec) * 1000000 +
                       (double)(e.tv_nsec - s.tv_nsec) / 1000;

    uint64_t all_tp = 0;
    for (int i = 0; i < kThreadCount; ++i) {
      all_tp += tp[i][0];
    }
    uint64_t cap = all_tp - pre_tp;
    pre_tp = all_tp;

    double all = 0, hit = 0;
    for (int i = 0; i < MAX_APP_THREAD; ++i) {
      all += (cache_hit[i] + cache_miss[i]);
      hit += cache_hit[i];
    }

    clock_gettime(CLOCK_REALTIME, &s);

    if (++count % 3 == 0) {
      cal_latency();
    }

    double per_node_tp = cap * 1.0 / microseconds;
    uint64_t cluster_tp = dsm->sum((uint64_t)(per_node_tp * 1000));

    printf("%d, throughput %.4f\n", dsm->getMyNodeID(), per_node_tp);

    printf("cluster throughput %.3f\n", cluster_tp / 1000.0);
    printf("cache hit rate: %lf\n", hit * 1.0 / all);
  }

  return 0;
}
