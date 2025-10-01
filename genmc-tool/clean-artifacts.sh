#!/bin/bash
#
# This script erases Job Artifacts for a project.
# To prevent new artifacts from accumulating, please use the:
#     "artifacts:expire_in" option in your .gitlab-ci.yml file
#
# This option is documented in the following link:
#     https://docs.gitlab.com/ee/ci/yaml/#artifactsexpire_in
#
# Based on the script created by Chris Arceneaux, in:
#     https://forum.gitlab.com/t/remove-all-artifact-no-expire-options/9274/8
#
# Dependencies:
#     curl, jq
#
# License:
#     Apache 2.0 License


# PROJECT VARIABLES
###################

# gitlab server (for MPI-SWS: gitlab.mpi-sws.org)
server="gitlab.mpi-sws.org"

# project_id, find it here: https://${server}/[organization]/[repository]
# (specifically for GenMC: https://gitlab.mpi-sws.org/michalis/genmc-tool)
project_id="1071"

# personal access token. If you don't have one, make one here:
# https://${server}/profile/personal_access_tokens
token="sHj7-pSo62PhJ3F3C1m5"


# RETRIEVE ARTIFACTS
# (NOTE: starts from page 2; modify L49 if desired)
###################################################

# Retrieving Jobs list page count
total_pages=$(curl -sD - -o /dev/null -X GET \
  "https://${server}/api/v4/projects/$project_id/jobs?per_page=100" \
  -H "PRIVATE-TOKEN: ${token}" | grep -Fi X-Total-Pages | sed 's/[^0-9]*//g')

# Creating list of Job IDs for the Project specified with Artifacts
job_ids=()
echo ""
echo "Creating list of all Jobs that currently have Artifacts..."
echo "Total Pages: ${total_pages}"
for ((i=2;i<=${total_pages};i++)) #starting with page 2 skipping most recent 100 Jobs
do
  echo "Processing Page: ${i}/${total_pages}"
  response=$(curl -s -X GET \
    "https://$server/api/v4/projects/$project_id/jobs?per_page=100&page=${i}" \
    -H "PRIVATE-TOKEN: ${token}")
  length=$(echo "${response}" | jq '. | length')
  for ((j=0;j<${length};j++))
  do
    if [[ $(echo "${response}" | jq ".[${j}].artifacts_file | length") > 0 ]]; then
        echo "Job found: $(echo "${response}" | jq ".[${j}].id")"
        job_ids+=($(echo "${response}" | jq ".[${j}].id"))
    fi
  done
done


# ERASE ARTIFACTS
#################

# Loop through each Job erasing the Artifact(s)
echo ""
echo "${#job_ids[@]} Jobs found. Commencing removal of Artifacts..."
for job_id in ${job_ids[@]};
do
  response=$(curl -s -X POST \
    "https://$server/api/v4/projects/$project_id/jobs/$job_id/erase" \
    -H "PRIVATE-TOKEN:${token}")
  echo "Processing Job ID: ${job_id} - Status: $(echo $response | jq '.status')"
done
