#include<iostream>
#include<vector>
using namespace std;
void merge(vector<int>& v1,vector<int>&v2,int m,int n){
    if(n==0)return;
    n--;m--;
    while(n>=0&&m>=0){
        if(v2[n]>v1[m]){
            v1[m+n+1] = v2[n];
            n--;
        }else{
            v1[m+n+1]=v1[m];
            m--;
        }
    }
    m=0;
    while(n>=0){
        v1[m+n+1] = v2[n];
        n--;
    }
}
int main(){
    vector<int> arr{2,5,78,88,0,0,0,0};
    vector<int> brr{6,7,89,90};
    vector<int> crr{3,5,7,9,11,0,0,0,0,0,0,0};
    vector<int> drr{4,7,9,23,45,67,99};
    vector<int> err{1,2,3,4,0,0,0,0};
    vector<int> frr{7,8,9,12};
     merge(err,frr,4,4);
    for(auto i :err){
        std::cout<<i<<" ";
    }
}
