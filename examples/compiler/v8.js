function compilerNested(a) { let s=0; for(let i=0;i<a.length;i++){ for(let j=0;j<i;j++){ if(a[j]>a[i]) s+=a[j]; else s-=a[i]; } } return s; }
function compilerSwitch(a) { let s=0; for(let i=0;i<a.length;i++){ switch(a[i]&7){case 0:s+=3;break;case 1:s-=2;break;case 2:s*=2;break;case 3:s^=9;break;default:s+=a[i];} } return s; }
function compilerSearch(a,n) { let low=0,high=a.length; while(low<high){let mid=(low+high)>>>1;if(a[mid]<n)low=mid+1;else high=mid;}return low; }
for(const f of [compilerNested,compilerSwitch,compilerSearch]) { %PrepareFunctionForOptimization(f); for(let k=0;k<20;k++) f([1,3,5,7,9],4); %OptimizeFunctionOnNextCall(f); f([1,3,5,7,9],4); }
