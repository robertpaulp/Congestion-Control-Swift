import os
import subprocess
import numpy as np
import matplotlib.pyplot as plt
import math

def plot_generic(axs, index_f1, index_f2, ylabel, xlabel, scale_y, num_cons, data_folder="tmp/t/"):
    for i in range(num_cons):
        x1, y1 = [], []
        # Citeste throughput-ul din fisierul aferent conexiunii i
        with open("{}{}.dat".format(data_folder, i)) as f:
            for line in f:
                values = line.split()
                x1.append(float(values[index_f1]))
                y1.append(float(values[index_f2]) * scale_y)
        axs.plot(x1, y1, '-', label='Conn {}'.format(i))
        axs.legend()
    
def plot_throughput(axs, num_cons):
    axs.set_ylabel('Throughput (Mbps)')
    axs.set_xlabel('Time (s)')
    # Valorile din fisieri sunt in byes asa ca le inmultim cu 8/1000000 ca sa trecem in Mb/s
    plot_generic(axs, 0, 12, 'Throughput', 'Time (s)', 8 / 1000000, num_cons)

def plot_cwnd(axs, num_cons):
    axs.set_ylabel('CWND (MSS)')
    axs.set_xlabel('Time (s)')
    plot_generic(axs, 0, 10, 'CWND (MSS)', 'Time (s)', 1/1500, num_cons)

def plot_router_queue(axs, max_queue_size, data_folder="tmp/t/"):
    axs.set_ylabel('Packets in Queue (MSS) ')
    axs.set_xlabel('Time (s)')
    x1, y1 = [], []
    with open("{}q.dat".format(data_folder)) as f:
        for line in f:
            values = line.split()
            x1.append(float(values[0]))
            y1.append(float(values[12]) * 1/1500)
        
        axs.hlines(y=max_queue_size, xmin=0, xmax=x1[-1], linewidth=2, color='r')
        axs.plot(x1, y1, '-')  

# Calcuelaza average-ul dintr-o coloana dintr-un fisier .dat
def avg_col(data_path, col, scale):
        x1 = []
        with open(data_path) as f:
            for line in f:
                values = line.split()
                x1.append(float(values[col]) * scale)
        return sum(x1)/(len(x1))

# Afiseaza throughput-ul pentru o conexiune si traseazao linie orizontala la valoarea ideala.
def plot_throughput2(axs, data_path, bandwidth):
    axs.set_ylabel('Throughput (Mbps)')
    axs.set_xlabel('Time (s)')
    x1 = []
    y1 = []
    with open(data_path) as f:
        for line in f:
            values = line.split()
            x1.append(float(values[0]))
            y1.append(float(values[12]) * 8 / 1000000)
            
    axs.hlines(y=bandwidth, xmin=0, xmax=x1[-1], linewidth=2, color='r')
    # Ca sa arata graficul mai bine, nu luam in considerare primele 4 tick-uri
    axs.plot(x1[4:], y1[4:], '-')
    
# Afiseaza throughput-ul dintr-un anumit folder .dat
def plot_throughput_from_data(axs, num_cons, data_folder):
    axs.set_ylabel('Throughput (Mbps)')
    axs.set_xlabel('Time (s)')
    # Valorile din fisieri sunt in byes asa ca le inmultim cu 8/1000000 ca sa trecem in Mb/s
    plot_generic(axs, 0, 12, 'Throughput', 'Time (s)', 8 / 1000000, num_cons, data_folder)

def throughput():
    # Main parameters
    MSS = 1500  # in bytes
    latency = [4, 1, 10]  # ms
    bandwidth = [100, 500, 1000]  # Mbps
    bdp = [int((i / 1000) * (j * 10**6)) for i, j in zip(latency, bandwidth)]  # in bits

    # Calculate BDP in MSS and queue size in MSS
    bdp_in_mss = [int(i / (MSS * 8)) for i in bdp]  # in packets
    queue_size_in_mss = [int(10 * i / (MSS * 8)) for i in bdp]  # 10 BDP queue size in MSS
    avg = []

    # Run for three different scenarios
    for i in range(3):
        data_folder = f"tmp/thr{i}/"
        os.makedirs(data_folder, exist_ok=True)
    
        # Run the simulations
        command = f"./htsim_dumbell_cc -conns 1 -end 20000 -log 100 -q {queue_size_in_mss[i]} " \
                  f"-latency {latency[i]} -bandwidth {bandwidth[i]} -ecn {bdp_in_mss[i]} {bdp_in_mss[i]}"
        subprocess.run(command, shell=True)
        
        # Calculate average column value
        avg_value = avg_col(f"tmp/thr{i}/0.dat", 12, 8 / 1000000)
        avg.append(avg_value)
        print(f"\n##### [RESULT] Average throughput {avg_value} vs {bandwidth[i]} expected")

    # Calculate the score for this category
    score_throughput = sum(avg[i] / bandwidth[i] for i in range(3)) * 100
    #f = open("testing.txt", "a")
    #f.write(f"+++++ Scorul pentru aceasta categorie: {score_throughput} puncte\n")
    #f.close()
    #print(f"+++++ Scorul pentru aceasta categorie: {score_throughput} puncte")

    return score_throughput

def fairness():
    # Main parameters
    MSS = 1500  # in bytes
    num_cons = [4, 8, 16]
    router_queue_size = 1000  # in MSS
    latency = [5, 2, 10]  # ms
    bandwidth = [50, 50, 50]  # Mbps
    bdp = [int((i / 1000) * (j * 10**6)) for i, j in zip(latency, bandwidth)]  # in bits

    # Calculate BDP in MSS
    bdp_in_mss = [int(i / (MSS * 8)) for i in bdp]  # in packets
    fairness = []

    # Run for three different scenarios
    for i in range(3):
        data_folder = f"tmp/fair{i}/"
        os.makedirs(data_folder, exist_ok=True)
    
        # Run the simulations
        command = f"./htsim_dumbell_cc -conns {num_cons[i]} -end 80000 -bandwidth {bandwidth[i]} " \
                  f"-log 100 -q {router_queue_size} -latency {latency[i]} -ecn {bdp_in_mss[i]} {bdp_in_mss[i]} -startdelta 500"
        subprocess.run(command, shell=True)

        # Calculate the fairness index
        avgs = []
        for j in range(num_cons[i]):
            # 8/10000000 to convert to Mbps
            avg_value = avg_col(f"tmp/fair{i}/{j}.dat", 12, 8 / 1000000)
            avgs.append(avg_value)
        fairness_index = min(avgs) / max(avgs)
        fairness.append(fairness_index)
        print(f"\n####### Min/Max average throughput: {min(avgs)}/{max(avgs)}")

    # Calculate the fairness score for this category
    score_fairness = sum(fairness) / 3 * 100 * 3
    #f = open("testing.txt", "a")
    #f.write(f"+++++ Scorul pentru aceasta categorie: {score_fairness} puncte\n")
    #f.close()
    #print(f"+++++ Scorul pentru aceasta categorie: {score_fairness} puncte")

    return score_fairness

def latency():
    # Main parameters
    MSS = 1500  # in bytes
    latency = [5, 5, 5]  # ms
    bandwidth = [10, 50, 100]  # Mbps
    bdp = [int((i / 1000) * (j * 10**6)) for i, j in zip(latency, bandwidth)]  # in bits

    # Calculate BDP in MSS and queue size in MSS
    bdp_in_mss = [int(i / (MSS * 8)) for i in bdp]  # in packets
    queue_size_in_mss = [10 * bdp_in_mss[i] for i in range(3)]  # 10 BDP queue size in MSS
    num_cons = [2, 4, 3]
    avgs = []

    # Run for three different scenarios
    for i in range(3):
        data_folder = f"tmp/lat{i}/"
        os.makedirs(data_folder, exist_ok=True)
    
        # Run the simulations
        command = f"./htsim_dumbell_cc -conns {num_cons[i]} -end 60000 -log 100 -q {queue_size_in_mss[i]} " \
                  f"-startdelta 200 -latency {latency[i]} -bandwidth {bandwidth[i]} " \
                  f"-ecn {bdp_in_mss[i]} {bdp_in_mss[i]}"
        subprocess.run(command, shell=True)
    
        # Calculate average column value
        avg_value = avg_col(f"tmp/lat{i}/q.dat", 12, 1/1500)
        avgs.append(avg_value)

    # Calculate the latency score for this category
    score_latency = 300 - (sum(avg / queue_size_in_mss[i] * 100 for i, avg in enumerate(avgs)))
    #f = open("testing.txt", "a")
    #f.write(f"+++++ Scorul pentru aceasta categorie: {score_latency} puncte\n")
    #f.close()
    #print(f"\n+++++ Scorul pentru aceasta categorie: {score_latency} puncte")

    return score_latency


def main():
    aditive_increase = np.arange(0.5, 1, 0.1)
    betta = np.arange(0.1, 0.5, 0.2)
    max_mdf = np.arange(0.1, 0.5, 0.2)
    max_score = 0
    best_ai = 0
    best_b = 0
    best_m = 0

    for ai in aditive_increase:
        for b in betta:
            for m in max_mdf:
                f = open("vars.txt", "w")
                f.write(f"{ai} {b} {m}")
                f.close()

                # make the simulation
                command = f"make"
                subprocess.run(command, shell=True)


                # Run the throughput category
                throughput_score = throughput()
                # Run the fairness category
                fairness_score = fairness()
                # Run the latency category
                latency_score = latency()

                # Calculate the total score
                total_score = throughput_score + fairness_score + latency_score
                f = open("testing.txt", "a")
                f.write(f"Toate scorurile: {throughput_score} {fairness_score} {latency_score} {total_score}\n")
                f.close()

                if total_score > max_score:
                    max_score = total_score
                    best_ai = ai
                    best_b = b
                    best_m = m

    print(f"Best score: {max_score} with ai: {best_ai}, b: {best_b}, m: {best_m}")

if __name__ == "__main__":
    main()