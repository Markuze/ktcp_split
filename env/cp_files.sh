GIT=https://xlr8vgn:fmPeuyzLR47y7ecJQTJV@github.com/groupvrg

ssh -i ${1} ${2} hostname
ssh -i ${1} ${2} rm -rf ~/ENV;
ssh -i ${1} ${2} mkdir -p ~/ENV;
ssh -i ${1} ${2} git clone $GIT/cbn-agents.git ENV/cbn-agents
ssh -i ${1} ${2} git clone $GIT/cbn-ci.git ENV/cbn-ci
scp -r -i ${1} `dirname $0`/setup*sh ${2}:ENV/
scp -r -i ${1} `dirname $0`/params_${3}.txt ${2}:ENV/params.txt
ssh -i ${1} ${2} ls ~/ENV

