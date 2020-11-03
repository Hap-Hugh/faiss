
import time
import os
import numpy as np
import faiss

from faiss.contrib.datasets import SynteticDataset


os.system("cat /proc/cpuinfo | grep -m1 'model name' ")

faiss.omp_set_num_threads(1)

def format_tab(x):
    return "\n".join("\t".join("%g" % xi for xi in row) for row in x)


for nq in 100, 10000:

    print("*********** nq=", nq)

    if nq == 100:
        nrun = 500
        unit = "ms"
    else:
        nrun = 20
        unit = "s"

    restab = []
    for d in 16, 32, 64, 128:

        print("========== d=", d)

        nb = 10000

        # d = 32

        ds = SynteticDataset(d, 0, nb, nq)

        print(ds)

        index = faiss.IndexFlatL2(d)

        index.add(ds.get_database())

        nrun = 10
        restab1 = []
        restab.append(restab1)
        for k in 1, 10, 100:
            times = []
            for run in range(nrun):
                t0 = time.time()
                index.search(ds.get_queries(), k)
                t1 = time.time()
                if run >= nrun // 5: # the rest is considered warmup
                    times.append((t1 - t0))
            times = np.array(times)

            if unit == "ms":
                times *= 1000
                print("search k=%3d t=%.3f ms (± %.4f)" % (
                    k, np.mean(times), np.std(times)))
            else:
                print("search k=%3d t=%.3f s (± %.4f)" % (
                    k, np.mean(times), np.std(times)))
            restab1.append(np.mean(times))

    print("restab=\n", format_tab(restab))

