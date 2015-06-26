Compilation instructions for TVC

# Steps 1 through 11 explain how to compile TVC.
# Step 12 onward explain how to deploy and run the compiled version on a single computer or cluster.

For more information see:
http://ioncommunity.lifetechnologies.com/community/products/torrent-variant-caller

# 1. Download source files

wget updates.iontorrent.com/tvc_standalone/tvc-4.4.3.tar.gz
wget updates.iontorrent.com/tvc_standalone/ion_gatk-4.4.3.tar.gz


# 2. Copy source files into build root directory

TVC_VERSION=tvc-4.4.3
ION_GATK_VERSION=ion_gatk-4.4.3

BUILD_ROOT_DIR=`mktemp -d`
cp $TVC_VERSION.tar.gz $BUILD_ROOT_DIR
cp $ION_GATK_VERSION.tar.gz $BUILD_ROOT_DIR


# 3. Install dependencies

# 3.1 RedHat/CentOS

yum -y install gcc-c++ cmake zlib-devel bzip2-devel bzip2 \
ncurses-devel python-simplejson java atlas-devel blas-devel lapack-devel redhat-lsb-core


# 3.2 Debian/Ubuntu

sudo aptitude install g++ cmake zlib1g-dev libbz2-dev libncurses-dev \
libatlas-dev liblapack-dev default-jre


# 3.3 cmake

# Required is cmake (>=2.8.0), you can check the installed version with e.g.:

#  $ cmake -version
#  cmake version 2.8.0

# Installation of a newer cmake version is only required if the installed version is older than 2.8.0

# To delete the old cmake package:

# Redhat/CentOS:
yum -y erase cmake

# Debian/Ubuntu:
aptitude purge cmake

cd ~
wget http://www.cmake.org/files/v2.8/cmake-2.8.11.2.tar.gz
tar xvzf cmake-2.8.11.2.tar.gz
cd cmake-2.8.11.2
./configure
make -j5
make install


# 4. build armadillo
cd $BUILD_ROOT_DIR
wget http://sourceforge.net/projects/arma/files/armadillo-4.300.8.tar.gz
tar xvzf armadillo-4.300.8.tar.gz
cd armadillo-4.300.8/
sed -i 's:^// #define ARMA_USE_LAPACK$:#define ARMA_USE_LAPACK:g' include/armadillo_bits/config.hpp
sed -i 's:^// #define ARMA_USE_BLAS$:#define ARMA_USE_BLAS:g'     include/armadillo_bits/config.hpp
cmake .
make -j4


# 5. build bamtools
cd $BUILD_ROOT_DIR
wget updates.iontorrent.com/updates/software/external/bamtools-2.3.0.20131211+git67178ae187.tar.gz
tar xvzf bamtools-2.3.0.20131211+git67178ae187.tar.gz
mkdir bamtools-2.3.0.20131211+git67178ae187-build
cd bamtools-2.3.0.20131211+git67178ae187-build
cmake ../bamtools-2.3.0.20131211+git67178ae187 -DCMAKE_BUILD_TYPE:STRING=RelWithDebInfo
make -j4


# 6. build vcftools
cd $BUILD_ROOT_DIR
wget http://downloads.sourceforge.net/project/vcftools/vcftools_0.1.11.tar.gz
tar xvzf vcftools_0.1.11.tar.gz
cd vcftools_0.1.11
make -j4


# 7. build htslib
cd $BUILD_ROOT_DIR
wget --no-check-certificate https://github.com/samtools/htslib/archive/1.1.tar.gz -O htslib-1.1.tar.gz
tar xvzf htslib-1.1.tar.gz
cd htslib-1.1
make -j4


# 8. build samtools
cd $BUILD_ROOT_DIR
wget http://downloads.sourceforge.net/project/samtools/samtools/0.1.19/samtools-0.1.19.tar.bz2
tar xvjf samtools-0.1.19.tar.bz2
cd samtools-0.1.19
make -j4


# 9. download ION-GATK
cd $BUILD_ROOT_DIR
tar xvzf $ION_GATK_VERSION.tar.gz


# 10. build TVC
cd $BUILD_ROOT_DIR
tar xvzf $TVC_VERSION.tar.gz
TVC_SOURCE_DIR=$BUILD_ROOT_DIR/$TVC_VERSION
DISTRIBUTION_CODENAME=`lsb_release -is`_`lsb_release -rs`_`uname -m`
TVC_INSTALL_DIR=$BUILD_ROOT_DIR/$TVC_VERSION-$DISTRIBUTION_CODENAME-binary
mkdir $TVC_VERSION-build
cd $TVC_VERSION-build
cmake $TVC_SOURCE_DIR -DCMAKE_INSTALL_PREFIX:PATH=$TVC_INSTALL_DIR -DCMAKE_BUILD_TYPE:STRING=RelWithDebInfo
make -j4 install



# 11. copy binaries into TVC_INSTALL_DIR
cd $BUILD_ROOT_DIR
cp -r $ION_GATK_VERSION/jar      $TVC_INSTALL_DIR/share/TVC/
cp vcftools_0.1.11/bin/vcftools  $TVC_INSTALL_DIR/bin/
cp htslib-1.1/tabix              $TVC_INSTALL_DIR/bin/
cp htslib-1.1/bgzip              $TVC_INSTALL_DIR/bin/
cp samtools-0.1.19/samtools      $TVC_INSTALL_DIR/bin/

tar cvzf $TVC_VERSION-$DISTRIBUTION_CODENAME-binary.tar.gz $TVC_VERSION-$DISTRIBUTION_CODENAME-binary


######################################################################################

# 12.1 Either use the TVC version from the (temporary) TVC_INSTALL_DIR directory

TVC_ROOT_DIR=$TVC_INSTALL_DIR


# 12.2 Or use the TVC binary version.

tar xvzf $TVC_VERSION-$DISTRIBUTION_CODENAME-binary.tar.gz
TVC_ROOT_DIR=`pwd`/$TVC_VERSION-$DISTRIBUTION_CODENAME-binary


# 13. export PATH, following tools are required: samtools vcftools bgzip tabix zip tvc
export PATH=$PATH:$TVC_ROOT_DIR/bin


# 14. adjust some file paths and invoke TVC

# Required are 1 reference, 2 bed files, 1 aligned bam file, and 1 tvc parameter file

# Example 1:

$TVC_ROOT_DIR/bin/variant_caller_pipeline.py \
    --input-bam       $TVC_ROOT_DIR/share/TVC/examples/example1/test.bam \
    --reference-fasta $TVC_ROOT_DIR/share/TVC/examples/example1/reference.fasta \
    --region-bed      $TVC_ROOT_DIR/share/TVC/examples/example1/test_merged_plain.bed \
    --primer-trim-bed $TVC_ROOT_DIR/share/TVC/examples/example1/test_unmerged_detail.bed

# Example 1 with specified parameter file and output directory:

$TVC_ROOT_DIR/bin/variant_caller_pipeline.py \
    --input-bam       $TVC_ROOT_DIR/share/TVC/examples/example1/test.bam \
    --reference-fasta $TVC_ROOT_DIR/share/TVC/examples/example1/reference.fasta \
    --region-bed      $TVC_ROOT_DIR/share/TVC/examples/example1/test_merged_plain.bed \
    --primer-trim-bed $TVC_ROOT_DIR/share/TVC/examples/example1/test_unmerged_detail.bed \
    --parameters-file $TVC_ROOT_DIR/share/TVC/pluginMedia/parameter_sets/ccp_somatic_lowstringency_pgm_parameters.json \
    --output-dir      /tmp/tvc_example1


# Example 2 (not included yet) :

# Required are 1 reference, 2 bed files, 1 aligned bam file, and 1 tvc parameter file
                REF=/mnt/TS/source/hg19/hg19.fasta
BED_UNMERGED_DETAIL=/mnt/TS/source/tvc_test_GB1-118-CSI/unmerged/detail/Exome_draft_Designed_20130531.bed
   BED_MERGED_PLAIN=/mnt/TS/source/tvc_test_GB1-118-CSI/merged/plain/Exome_draft_Designed_20130531.bed
                BAM=/mnt/TS/source/tvc_test_GB1-118-CSI/IonXpress_045_rawlib.bam
          TVC_PARAM=$TVC_ROOT_DIR/share/TVC/pluginMedia/configs/germline_low_stringency_proton.json

TMP_DIR=`mktemp -d`

$TVC_ROOT_DIR/bin/variant_caller_pipeline.py \
    --parameters-file $TVC_PARAM \
    --input-bam $BAM \
    --reference-fasta $REF \
    --region-bed $BED_MERGED_PLAIN \
    --primer-trim-bed $BED_UNMERGED_DETAIL \
    --postprocessed-bam $TMP_DIR/trimmed.bam \
    --output-dir $TMP_DIR


# Example 3 (hotspot) (not included yet):

                REF=/mnt/TS/source/hg19/hg19.fasta
BED_UNMERGED_DETAIL=/mnt/TS/source/tvc_test_Z06-506-CCP/unmerged_detail_CCP.20131001.designed.bed
   BED_MERGED_PLAIN=/mnt/TS/source/tvc_test_Z06-506-CCP/merged_plain_CCP.20131001.designed.bed
                BAM=/mnt/TS/source/tvc_test_Z06-506-CCP/IonXpress_019_rawlib.bam
          TVC_PARAM=/mnt/TS/source/tvc_test_Z06-506-CCP/local_parameters.json
            HOTSPOT=/mnt/TS/source/tvc_test_Z06-506-CCP/hotspot.vcf

TMP_DIR=`mktemp -d`

time $TVC_ROOT_DIR/bin/variant_caller_pipeline.py \
    --parameters-file $TVC_PARAM \
    --input-bam $BAM \
    --reference-fasta $REF \
    --region-bed $BED_MERGED_PLAIN \
    --primer-trim-bed $BED_UNMERGED_DETAIL \
    --hotspot-vcf $HOTSPOT \
    --postprocessed-bam $TMP_DIR/trimmed.bam \
    --output-dir $TMP_DIR

