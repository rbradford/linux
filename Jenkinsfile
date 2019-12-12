pipeline {
    agent none
    options { skipDefaultCheckout() }
    stages {
        stage ('Master build') {
            agent { node { label 'master' } }
            stages {
                stage ('Check for RFC/WIP builds') {
                    when {
                        changeRequest comparator: 'REGEXP', title: '.*(rfc|RFC|wip|WIP).*'
                        beforeAgent true
                    }
                    steps {
                        error("Failing as this is marked as a WIP or RFC PR.")
                    }
                }
                stage ('Cancel older builds') {
                    steps {
                        cancelPreviousBuilds()
                    }
                }
            }
        }
        stage ('Worker build') {
            agent {
                //node { label 'bionic' }
                agent { node { label 'master' } }
            }
            options {
                timeout(time: 1, unit: 'HOURS')
            }
            stages {
                stage ('Checkout') {
                    steps {
                        checkout([
                            $class: 'GitSCM',
                            branches: scm.branches,
                            doGenerateSubmoduleConfigurations: scm.doGenerateSubmoduleConfigurations,
                            extensions: scm.extensions + [[$class: 'CloneOption', noTags: false, reference: '']],
                            submoduleCfg: [],
                            userRemoteConfigs: scm.userRemoteConfigs
                        ])
                    }
                }
                stage ('Install system packages') {
                    steps {
                        sh "sudo DEBIAN_FRONTEND=noninteractive apt-get install -yq build-essential flex bison libelf-dev"
                    }
                }
                stage ('Configure kernel') {
                    steps {
                        sh "wget https://raw.githubusercontent.com/cloud-hypervisor/cloud-hypervisor/master/resources/linux-virtio-fs-virtio-iommu-config"
                        sh "cp linux-virtio-fs-virtio-iommu-config .config"
                        sh "make olddefconfig"
                    }
                }
                stage ('Build kernel') {
                    steps {
                        sh "make bzImage -j `nproc`"
                    }
                }
                stage ('Upload to storage') {
                    steps {
                        sh "git describe -a"
                        sh "cp arch/x86/boot/bzImage ."
                        azureUpload storageCredentialId: 'cloud-hypervisor-storage-account',
                                                filesPath: "vmlinux",
                                                storageType: 'blobstorage',
                                                containerName: 'kernels'
                        azureUpload storageCredentialId: 'cloud-hypervisor-storage-account',
                                                filesPath: "bzImage",
                                                storageType: 'blobstorage',
                                                containerName: 'kernels'
                    }
                }
            }
        }
    }
}

def cancelPreviousBuilds() {
    // Check for other instances of this particular build, cancel any that are older than the current one
    def jobName = env.JOB_NAME
        def currentBuildNumber = env.BUILD_NUMBER.toInteger()
        def currentJob = Jenkins.instance.getItemByFullName(jobName)

        // Loop through all instances of this particular job/branch
        for (def build : currentJob.builds) {
            if (build.isBuilding() && (build.number.toInteger() < currentBuildNumber)) {
                echo "Older build still queued. Sending kill signal to build number: ${build.number}"
                build.doStop()
            }
        }
}
