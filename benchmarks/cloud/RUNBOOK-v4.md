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
| P1, P2 (pairs) | trace replay ([v4-replay](../../docs/v4-replay-preregistration.md)): Azure on both pairs, then Huawei, DAS2, LPC-EGEE | 2 × (`c8i.2xlarge` + `c8i.xlarge`) | about 85 h | replay binary not yet written |

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

## Later lanes

- **Baselines** (same host, after E5 and E6): `benchmarks/run_prodflow.sh` with `v4-baselines.json` once that
  pre-registration is frozen; add it to `run_all.sh` as `e7` in the same way.
- **Trace replay on the pairs:** a pair is launched as in README.md sections 1 to 4. Assignments (fixed in the configs):
  P1 takes Azure load 0.5, then Huawei and DAS2; P2 takes Azure loads 0.8 and 0.95, then LPC-EGEE and the E2
  repeats. The details are added when the replay binary exists.
- Terminate lane S's host before launching the second pair if you want the quota for something else.
