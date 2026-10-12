# AWS runbook for the v3 cloud experiments (#46, #47)

This runbook covers four experiments, frozen in two pre-registrations:

| Experiment | Pre-registration | Hosts |
|---|---|---|
| E1: v2 production flow replicated | [v3-cloud](../../docs/v3-cloud-preregistration.md) | client only |
| E2: 30-minute run, broker on its own host | v3-cloud | broker + client |
| E3: Harbinger vs RabbitMQ and Redis | [v3-brokers](../../docs/v3-brokers-preregistration.md) | broker + client |
| E4: broker cost | v3-brokers | broker + client |

**Your account, your commands.** You run every AWS command below with your own credentials; nothing in this
repository holds credentials. The commands use the AWS CLI v2 and region `us-east-1`.

## Cost and caps

| Item | Choice | On-demand price (us-east-1) |
|---|---|---|
| Client host | `c8i.2xlarge`: 8 vCPU, 16 GiB | ~$0.37/h |
| Broker host | `c8i.xlarge`: 4 vCPU, 8 GiB | ~$0.19/h |
| Disks | 30 GB gp3 each | ~$0.005/h each |

- **Debug pass:** about 1 h. **Registered runs:** about 15 h (E4 ~1 h, E3 ~5 h, E2 ~5 h, E1 ~4.5 h).
- **Expected total:** under $15 including setup. Set a **$50 budget alert** in the AWS Billing console before
  launching.
- **Spend cap.** `setup_host.sh` makes each instance power itself off after 20 h. Instances are launched with shutdown
  behaviour `stop`, so the disk and its results survive and compute billing ends.
- **Prices.** Check the current prices; c8i availability varies by zone. If c8i is unavailable, use c7i of the same
  sizes and record the change.

## 1. Network

Only you can reach the hosts over SSH. Inside the security group the hosts reach each other on any port, for SSH and
the broker ports. The brokers listen on all interfaces, which stays private to the group.

```bash
export AWS_REGION=us-east-1 MYIP=$(curl -s https://checkip.amazonaws.com)/32 KEY=<your key pair name>
aws ec2 create-placement-group --group-name harbinger-bench --strategy cluster
SG=$(aws ec2 create-security-group --group-name harbinger-bench --description "Harbinger benchmark hosts" \
  --query GroupId --output text)
aws ec2 authorize-security-group-ingress --group-id "$SG" --protocol tcp --port 22 --cidr "$MYIP"
aws ec2 authorize-security-group-ingress --group-id "$SG" --protocol -1 --source-group "$SG"
```

## 2. Launch both hosts

Both hosts use Ubuntu 24.04 (the current Canonical AMI, from SSM) in the placement group.

```bash
AMI=$(aws ssm get-parameter --name /aws/service/canonical/ubuntu/server/24.04/stable/current/amd64/hvm/ebs-gp3/ami-id \
  --query Parameter.Value --output text)
launch() {  # launch NAME TYPE
  aws ec2 run-instances --image-id "$AMI" --instance-type "$2" --key-name "$KEY" --security-group-ids "$SG" \
    --placement GroupName=harbinger-bench --instance-initiated-shutdown-behavior stop \
    --block-device-mappings 'DeviceName=/dev/sda1,Ebs={VolumeSize=30,VolumeType=gp3,DeleteOnTermination=true}' \
    --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=$1},{Key=project,Value=harbinger-bench}]" \
    --query 'Instances[0].InstanceId' --output text
}
CLIENT=$(launch harbinger-client c8i.2xlarge); BROKER=$(launch harbinger-broker c8i.xlarge)
aws ec2 wait instance-running --instance-ids "$CLIENT" "$BROKER"
aws ec2 describe-instances --instance-ids "$CLIENT" "$BROKER" \
  --query 'Reservations[].Instances[].[Tags[?Key==`Name`]|[0].Value,PublicIpAddress,PrivateIpAddress,Placement.AvailabilityZone]' \
  --output table
```

Both hosts must report the same availability zone. Note each host's public and private IP.

## 3. Prepare both hosts

```bash
for ip in <client public IP> <broker public IP>; do
  scp benchmarks/cloud/setup_host.sh ubuntu@$ip: && ssh ubuntu@$ip bash setup_host.sh 20
done
```

**Client SSH key for the broker host.** The client host starts brokers over SSH:

```bash
ssh ubuntu@<client public IP> 'ssh-keygen -q -t ed25519 -N "" -f ~/.ssh/id_ed25519 && cat ~/.ssh/id_ed25519.pub' \
  | ssh ubuntu@<broker public IP> 'cat >> ~/.ssh/authorized_keys'
ssh ubuntu@<client public IP> 'ssh -o StrictHostKeyChecking=accept-new ubuntu@<broker private IP> true && echo ok'
```

## 4. Debug pass (before freezing)

On the client host, inside `tmux`, build the image once and give the broker host the identical image:

```bash
git clone https://github.com/divyanshusingh2903/predictive-multi-level-message-queue.git harbinger && cd harbinger
git checkout <branch or commit under test>
docker build -f benchmarks/docker/Dockerfile -t harbinger-bench .          # ~10 minutes
docker save harbinger-bench | ssh ubuntu@<broker private IP> docker load
BROKER_SSH=ubuntu@<broker private IP> BROKER_HOST=<broker private IP> benchmarks/cloud/run_all.sh debug ~/results
```

`debug` mode runs seed 7, which is not registered, with short durations:
- E4: one 10-second repetition;
- E3: all 8 arms, 60 s each;
- E2: `fifo` and `predictive` for 10 minutes each (the #26 debug runs);
- E1: `fifo` and `predictive`, 60 s each.

**Check in `~/results/debug-*/`:**
- Every `run.json` exists, with `completed` equal to `messages`.
- There is no `worker_error`, and no "no broker.json" or "merge failed" line in the logs.
- `broker-versions.txt` and `client-versions.txt` match, as do the image IDs in `manifest.txt`.
- The E2 runs drained fully. If the 10-minute FIFO run barely drained, raise `drain_s` in `v3-cloud-e2.json` before
  freezing.

The debug results are disclosed in the pre-registrations and are not analysed further.

## 5. Freeze

On your machine, in one commit, then push:
- set `frozen_on` in the four configs (`v3-cloud-e1`, `v3-cloud-e2`, `v3-brokers-prodflow`, `v3-brokers-cost`);
- remove the DRAFT notices;
- add what the debug pass showed.

## 6. Registered runs

On the client host, check out the freeze commit, rebuild and re-ship the image (step 4), then:

```bash
BROKER_SSH=ubuntu@<broker private IP> BROKER_HOST=<broker private IP> benchmarks/cloud/run_all.sh registered ~/results
```

`registered` mode:
- refuses a dirty tree or unfrozen configs;
- refuses to run if the two hosts' images differ;
- runs E4, E3, E2, then E1 (on the client alone);
- writes each report;
- packs everything into `~/results/registered-<commit>.tar.gz` with its SHA-256.

E1 needs no broker host. You can stop the broker instance once `progress.log` shows E1 has started.

## 7. Bring results back, then tear down

```bash
scp 'ubuntu@<client public IP>:results/*.tar.gz*' . && sha256sum -c registered-*.tar.gz.sha256
aws ec2 terminate-instances --instance-ids "$CLIENT" "$BROKER"
aws ec2 wait instance-terminated --instance-ids "$CLIENT" "$BROKER"
aws ec2 delete-security-group --group-id "$SG"
aws ec2 delete-placement-group --group-name harbinger-bench
aws ec2 describe-volumes --filters Name=tag:project,Values=harbinger-bench --query 'Volumes[].VolumeId'  # expect []
```

**What goes where.**
- **In git:** per-run `run.json`, the reports, the manifests and `SHA256SUMS`, as for earlier results.
- **Outside git:** the raw `messages.tsv` files, in the tarball. Archive the tarball (for example as a GitHub release
  asset) and record its hash in the results README.
- **Cost:** record the actual cost from the Billing console in the results README (#26).

## Tested so far

- **Local debug pass:** run on one machine with both "hosts" local and an SSH stand-in (`SSH=<script>`), which runs
  the remote commands with the same quoting.
- **Not yet tested:** the AWS commands themselves (sections 1–3 and 7), and real SSH between two hosts. The debug pass
  in section 4 is the first real test of the latter.
