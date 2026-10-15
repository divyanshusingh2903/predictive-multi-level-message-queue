# AWS runbook for the v4 experiments

Extends [README.md](README.md), the runbook for the v3 cloud experiments. Read that first for the network setup,
the host preparation and the image build; this file says what is new in v4 and gives the commands in order.

**Your account, your commands.** You run every AWS command with your own credentials. Nothing in this repository holds
credentials. If the CLI says the session has expired, run `aws login`.

## Scope

E1 to E4 are **not** re-run. The v4 work on AWS is:

| Lane | Experiments | Hosts | Time | Status |
|---|---|---|---|---|
| **S** (single host) | E5 aging and E6 size bins ([v4-cloud-reruns](../../docs/v4-cloud-reruns-preregistration.md)) | 1 × `c8i.2xlarge` | about 3 h | **ready to run** |
| S | baselines and ablation ([v4-baselines](../../docs/v4-baselines-preregistration.md)) | same host, after E5, E6 | about 8.4 h | arms built; pre-registration not frozen |
| R (two pairs) | trace replay ([v4-replay](../../docs/v4-replay-preregistration.md)): Huawei at three loads, Azure at three loads (gate arms at the edges), both sharded across the pairs | 2 × (`c8i.2xlarge` + `c8i.xlarge`) | about 90 h of runs, about 42 to 49 h on each pair | harness and runner written and tested locally; **AWS smoke run next** |

## Quota

A broker-client pair is 12 vCPU (8 + 4) and a single host is 8. Your quota of 32 vCPU is exactly **2 pairs and 1 single
host (12 + 12 + 8)**, with nothing left over. Lane S therefore can run at the same time as both pairs; a fourth
machine of any size cannot. To start with lane S alone, you need only 8 vCPU.

Check the current usage before launching: Service Quotas, EC2, "Running On-Demand Standard (A, C, D, H, I, M, R, T, Z)
instances". Terminated instances release their vCPU within minutes.

## Cost

At the runbook's on-demand prices (check the current ones): a single host is about $0.37/h, a pair about $0.56/h.
Lane S is about 3 h for E5 and E6, plus about 8.4 h for the baselines, plus setup: **about $5 to $6** in all. Set a
**$50 budget alert** in the AWS Billing console before launching, as in the v3 runbook. `setup_host.sh` powers each
instance off after the hours you give it (default 20; give 14 for lane S), and instances are launched with shutdown
behaviour `stop`, so a forgotten host stops billing compute.

## Lane S: E5 and E6, step by step

**1. Network and key pair.** As README.md section 1. A single host needs only your SSH access (the security-group rule
for the group's own traffic is harmless). Use the same placement group and security group as before if they still
exist (`aws ec2 describe-security-groups --group-names harbinger-bench`).

**2. Launch one host.**

```bash
export AWS_REGION=us-east-1 MYIP=$(curl -s https://checkip.amazonaws.com)/32 KEY=<your key pair name>
SG=$(aws ec2 describe-security-groups --group-names harbinger-bench --query 'SecurityGroups[0].GroupId' --output text)
AMI=$(aws ssm get-parameter --name /aws/service/canonical/ubuntu/server/24.04/stable/current/amd64/hvm/ebs-gp3/ami-id \
  --query Parameter.Value --output text)
HOST=$(aws ec2 run-instances --image-id "$AMI" --instance-type c8i.2xlarge --key-name "$KEY" --security-group-ids "$SG" \
  --placement GroupName=harbinger-bench --instance-initiated-shutdown-behavior stop \
  --block-device-mappings 'DeviceName=/dev/sda1,Ebs={VolumeSize=30,VolumeType=gp3,DeleteOnTermination=true}' \
  --tag-specifications 'ResourceType=instance,Tags=[{Key=Name,Value=harbinger-lane-s},{Key=project,Value=harbinger-bench}]' \
  --query 'Instances[0].InstanceId' --output text)
aws ec2 wait instance-running --instance-ids "$HOST"
aws ec2 describe-instances --instance-ids "$HOST" --query 'Reservations[0].Instances[0].[PublicIpAddress,Placement.AvailabilityZone]' --output text
```

If the placement group no longer exists, create it first (README.md section 1) or omit `--placement`; record the choice
in the manifest note.

**3. Prepare the host** (README.md section 3, one host):

```bash
scp benchmarks/cloud/setup_host.sh ubuntu@<public IP>: && ssh ubuntu@<public IP> bash setup_host.sh 14
```

Log out and back in, so the docker group applies.

**4. Build the image on the host, inside `tmux`:**

```bash
git clone https://github.com/divyanshusingh2903/predictive-multi-level-message-queue.git harbinger && cd harbinger
git checkout <the commit under test>
docker build -f benchmarks/docker/Dockerfile -t harbinger-bench .          # about 10 minutes
```

**5. Debug pass** (before freezing; seed 7, which is not registered, 60 s per arm, about 5 minutes):

```bash
EXPERIMENTS="e5 e6" benchmarks/cloud/run_all.sh debug ~/results
```

Check in `~/results/debug-*/e5` and `e6`: every `run.json` exists with `completed` equal to `messages`, and no `dlq` or
`submit_errors`. Do not read latencies; the debug results are disclosed in the pre-registration and not analysed.

**6. Freeze (on your machine).** In one commit: set `frozen_on` in `v4-cloud-aging.json` and `v4-cloud-sizebins.json`,
remove the DRAFT notices from `docs/v4-cloud-reruns-preregistration.md`, and add what the debug pass showed. Push.

**7. Registered runs** (on the host, from the freeze commit; the tree must be clean and the image rebuilt from it):

```bash
git pull && git checkout <freeze commit> && docker build -f benchmarks/docker/Dockerfile -t harbinger-bench .
EXPERIMENTS="e5 e6" benchmarks/cloud/run_all.sh registered ~/results
```

It writes `~/results/registered-<commit>-e5-e6/`, with `e5/` and `e6/` reports, the manifest and a
`.tar.gz` with its SHA-256.

**8. Bring results back and tear down** (README.md section 7):

```bash
scp 'ubuntu@<public IP>:results/*e5-e6*.tar.gz*' . && sha256sum -c *e5-e6*.tar.gz.sha256
aws ec2 terminate-instances --instance-ids "$HOST" && aws ec2 wait instance-terminated --instance-ids "$HOST"
```

Then place the raw tree and its `SHA256SUMS` as for `v3-aws`, and give me the path; I will write the results
directory and the paper's numbers.

## Lane R: trace replay on two pairs

Replays the Huawei 2025 R5 trace (all three loads, f = 1) and the Azure Functions 2021 trace (all three loads, f = 4.5)
through the real broker, to replace the simulator's trace results (which overstated the mean-latency reduction by about
40% on the production flow, `v4-simvalidation-1`). The experiment is
[v4-replay](../../docs/v4-replay-preregistration.md); the harness is `harbinger_replay`, the runner
`benchmarks/run_replay.sh`, the analysis `benchmarks/replay_report.py`.

### What runs, and how long

| Trace | Config | Cells | One cell | Total |
|---|---|---|---|---|
| Azure 2021 | `v4-replay-azure2021.json` | 11 cells (5 arms at 0.8, 3 gate arms at 0.5 and 0.95), 1 repeat, plus the probe | 7.94 h (0.5), 4.96 h (0.8), 4.18 h (0.95); probe 2 min | 61.2 h |
| Huawei R5 | `v4-replay-huawei2025-r5.json` | 7 arms × 3 loads × 3 repeats, plus the probe | 39, 24 and 20 min (loads 0.5, 0.8, 0.95) | about 29 h |

Each experiment is split across both pairs with `SHARD=0/2` and `SHARD=1/2`: a shard takes the cells at even or odd
positions of the matrix (repeat, then load, then arm). Azure's shards are 34.2 h and 27.0 h. To even the pairs out,
**pair 1 takes Azure `0/2` and Huawei `1/2`, and pair 2 takes Azure `1/2` and Huawei `0/2`**: about 49 h and 42 h. **Run Azure
first** (its cells are the longest); the runner resumes, so the order costs nothing.

### Cost and quota

A pair is 12 vCPU (8 + 4): two pairs are 24 of your 32, which leaves lane S's 8. Lane S can keep running. At the runbook's
prices a pair is about $0.56/h, so two pairs for about 45 h on average is about **$50 plus disks**. Set a **$100 budget alert** before
launching. Give `setup_host.sh` **60** hours (the spend cap): an instance that powers itself off keeps its disk, and the
runner resumes where it stopped (see *If something stops*).

### 1. Launch two pairs

As README.md sections 1 to 3, with these differences: **one placement group per pair**, and names that carry the pair.

```bash
export AWS_REGION=us-east-1 MYIP=$(curl -s https://checkip.amazonaws.com)/32 KEY=<your key pair name>
SG=$(aws ec2 describe-security-groups --group-names harbinger-bench --query 'SecurityGroups[0].GroupId' --output text)
AMI=$(aws ssm get-parameter --name /aws/service/canonical/ubuntu/server/24.04/stable/current/amd64/hvm/ebs-gp3/ami-id \
  --query Parameter.Value --output text)
launch() {  # launch NAME TYPE PLACEMENT_GROUP
  aws ec2 run-instances --image-id "$AMI" --instance-type "$2" --key-name "$KEY" --security-group-ids "$SG" \
    --placement GroupName=$3 --instance-initiated-shutdown-behavior stop \
    --block-device-mappings 'DeviceName=/dev/sda1,Ebs={VolumeSize=30,VolumeType=gp3,DeleteOnTermination=true}' \
    --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=$1},{Key=project,Value=harbinger-bench}]" \
    --query 'Instances[0].InstanceId' --output text
}
for pair in p1 p2; do
  aws ec2 create-placement-group --group-name harbinger-replay-$pair --strategy cluster
  declare "CLIENT_$pair=$(launch harbinger-replay-$pair-client c8i.2xlarge harbinger-replay-$pair)"
  declare "BROKER_$pair=$(launch harbinger-replay-$pair-broker c8i.xlarge harbinger-replay-$pair)"
done
aws ec2 wait instance-running --instance-ids "$CLIENT_p1" "$BROKER_p1" "$CLIENT_p2" "$BROKER_p2"
```

If the security group does not exist any more, create it as README.md section 1. Within a pair, both hosts must report the
same availability zone (`describe-instances`, as in README.md section 2). Note each host's public and private IP.

Prepare all four hosts and give each client an SSH key for its broker host, exactly as README.md section 3, with 60 as the
spend-cap hours:

```bash
scp benchmarks/cloud/setup_host.sh ubuntu@<each public IP>: && ssh ubuntu@<each public IP> bash setup_host.sh 60
```

### 2. Put the traces on the client hosts

The converted traces are not in the repository. Copy them from the machine that has them to **both clients** (about
500 MB; the broker hosts never read a trace) and check the hashes against the configs:

```bash
for ip in <p1 client public IP> <p2 client public IP>; do
  scp ~/data/azure/azure2021.csv ~/data/huawei2025/huawei2025-r5.csv ubuntu@$ip:
  ssh ubuntu@$ip 'sha256sum azure2021.csv huawei2025-r5.csv'
done
# expected: 07e6c5c1d977896f97ff... azure2021.csv     e860c7a699670af200e0... huawei2025-r5.csv   (full values in the configs)
```

`run_replay.sh registered` refuses a trace whose SHA-256 is not the config's `converted_sha256`.

### 3. Build the image on each pair

Per pair, on the client host inside `tmux`, as README.md section 4:

```bash
git clone https://github.com/divyanshusingh2903/predictive-multi-level-message-queue.git harbinger && cd harbinger
git checkout <commit under test>
docker build -f benchmarks/docker/Dockerfile -t harbinger-bench .          # about 10 minutes; builds harbinger_replay too
docker save harbinger-bench | ssh ubuntu@<broker private IP> docker load
```

### 4. Smoke run (before freezing)

The pre-registration allows a smoke run only on a synthetic trace, and only to see whether the pipeline runs. Do this on
**one pair first** and, once it passes, on the other. It exercises (the Docker path and split roles already passed locally with a stand-in ssh, so what is new here is real
ssh and two hosts) the Docker path, the split roles, the warm snapshot
reaching the remote broker, the retry rule, the probe and the report, with all seven arms (about 15 minutes):

```bash
python3 -m benchmarks.make_replay_smoke ~/smoke            # writes smoke-trace.csv and smoke-config.json
BROKER_SSH=ubuntu@<broker private IP> BROKER_HOST=<broker private IP> \
  benchmarks/run_replay.sh debug ~/smoke/smoke-config.json ~/smoke/results ~/smoke/prep ~/smoke/smoke-trace.csv
python3 -m benchmarks.replay_report ~/smoke/smoke-config.json ~/smoke/results
```

Look only at these, and **do not read latencies or gates**:
- every `~/smoke/results/<arm>/0.5/rep-1/run.json` exists, with `completed` equal to `messages`, `dlq` 0, `submit_errors` 0,
  `leases_lost` 0, and `timing.lateness_p99_ms` at most 10;
- `progress.log` has no "does not count" line, and no "no broker.json" or "merge failed" line;
- the warm arms show the snapshot at work: in `predictive_warm/0.5/rep-1/run.json`, `broker.routing.outcomes.predicted` is
  most of `messages`, and `predictive_cold` shows `unready` or `cold_key` for the early part;
- `_probe/fifo/0.1/rep-1/run.json` exists, and the report ran.

Then run the same smoke embedded on one host (`benchmarks/run_replay.sh debug ...` without `BROKER_SSH`) only if the split
run found a problem you want to separate from the network.

### 5. Freeze (on your machine)

In one commit: set `frozen_on` in `v4-replay-huawei2025-r5.json` and `v4-replay-azure2021.json`, remove the DRAFT notices
from `docs/v4-replay-preregistration.md`, resolve its *confirm before freezing* choices, and record what the smoke run
showed (pipeline only). Push. Registered runs refuse an unfrozen config and a dirty tree.

### 6. Registered runs

On each pair's client host, inside `tmux`, from the freeze commit (the tree must be clean and the image rebuilt from it):

```bash
git pull && git checkout <freeze commit> && docker build -f benchmarks/docker/Dockerfile -t harbinger-bench .
docker save harbinger-bench | ssh ubuntu@<broker private IP> docker load
export BROKER_SSH=ubuntu@<broker private IP> BROKER_HOST=<broker private IP>
SHARD=<Azure: 0/2 on pair 1, 1/2 on pair 2> benchmarks/run_replay.sh registered benchmarks/configs/v4-replay-azure2021.json \
  ~/results/azure ~/prep-azure ~/azure2021.csv
SHARD=<Huawei: 1/2 on pair 1, 0/2 on pair 2> benchmarks/run_replay.sh registered benchmarks/configs/v4-replay-huawei2025-r5.json \
  ~/results/huawei ~/prep-huawei ~/huawei2025-r5.csv
```

The first call of each experiment runs `prepare` (a few seconds) and writes the plan and the warm snapshots. **Check that the
two pairs prepared the same thing:**

```bash
ssh ubuntu@<each client> 'cd ~ && sha256sum prep-azure/plan.json prep-azure/snapshot-*.bin prep-huawei/plan.json prep-huawei/snapshot-*.bin'
```

The checksums must be identical across the pairs; if not, stop and find out why before spending the hours.

Progress is in `~/results/<trace>/progress.log`: one line per cell, and a "does not count" line if a run was kept as
`.failed` and repeated. Cells of about 5 h (Azure) show nothing between their start and end lines; that is normal.

### If something stops

- **The instance powered off (spend cap) or was stopped:** `aws ec2 start-instances --instance-ids <id>` for the client
  and broker, log in, `tmux`, and run **the same command again**. Finished cells are skipped; the cell that was running is
  repeated from the start (its directory has no `run.json`). Do not delete anything.
- **The IP changed after a restart:** if `BROKER_HOST` is the broker's private IP it does not.
- **A cell is kept as `rep-<n>.failed`:** the runner already ran it once more. If the second attempt also does not count,
  both are kept and the report lists them; do not rerun by hand without telling me, as the pre-registration allows one repeat.
- **A broker container is stuck:** `ssh <broker> docker rm -f hb-replay-broker`, then rerun the same command.

### 7. Bring results back and tear down

On each client, make one archive per trace and its checksum, then copy them to your machine and verify:

```bash
cd ~/results && for t in azure huawei; do tar czf $t-<pair>.tar.gz $t && sha256sum $t-<pair>.tar.gz > $t-<pair>.tar.gz.sha256; done
scp 'ubuntu@<client public IP>:results/*.tar.gz*' . && sha256sum -c *.sha256
for pair in p1 p2; do c=CLIENT_$pair; b=BROKER_$pair; aws ec2 terminate-instances --instance-ids "${!c}" "${!b}"; done
aws ec2 delete-placement-group --group-name harbinger-replay-p1; aws ec2 delete-placement-group --group-name harbinger-replay-p2
```

Unpack both pairs' trees for a trace into one directory (the shards do not overlap, so the trees merge without conflict),
place it with its `SHA256SUMS` as for `v3-aws`, and run the analysis:

```bash
python3 -m benchmarks.replay_report benchmarks/configs/v4-replay-huawei2025-r5.json <merged huawei tree> \
  --simulation benchmarks/results/v3-traces/huawei2025-r5/simulation.json \
  --simulation-summary benchmarks/results/v3-traces/huawei2025-r5/summary.json
python3 -m benchmarks.replay_report benchmarks/configs/v4-replay-azure2021.json <merged azure tree> \
  --simulation benchmarks/results/v3-traces/azure2021/simulation.json \
  --simulation-summary benchmarks/results/v3-traces/azure2021/summary.json
```

Give me the path and I will write the results directory and the paper's numbers.

### What this lane does not do

DAS2-fs2 and LPC-EGEE (time scales 300 and 400, aging constants of 1 to 17 ms) are not in this lane. They are a different
experiment from Huawei and Azure, because scheduler jitter and the broker's 100 ms pull wake-up are comparable to their
constants; they need a calibration run first and would be reported as exploratory.

## Later lanes

- **Baselines, ablation and aging isolation** (`e7`, [v4-baselines](../../docs/v4-baselines-preregistration.md)): one
  `c8i.2xlarge` alone, 11 arms on ten seeds, about 9.4 h and about $4. It fits beside the two replay pairs: 24 + 8 = 32 vCPU,
  with nothing to spare. The lane S host can be reused (`aws ec2 start-instances`; its public IP changes), with the image
  rebuilt from the commit under test:

  ```bash
  git pull && git checkout <commit under test> && docker build -f benchmarks/docker/Dockerfile -t harbinger-bench .
  EXPERIMENTS="e7" benchmarks/cloud/run_all.sh debug ~/results        # all 11 arms, seed 7, 60 s each; about 15 minutes
  # freeze (set frozen_on in v4-baselines.json, remove the DRAFT notice from the pre-registration), push, then on the host:
  git pull && git checkout <freeze commit> && docker build -f benchmarks/docker/Dockerfile -t harbinger-bench .
  EXPERIMENTS="e7" benchmarks/cloud/run_all.sh registered ~/results   # writes ~/results/registered-<commit>-e7/
  ```

  Read in the debug pass only that every `run.json` has `completed` equal to `messages` and no `dlq` or `submit_errors`.
- **Trace replay on the pairs:** see Lane R below.
- Terminate lane S's host before launching the second pair if you want the quota for something else.
